#pragma once

#include <ck/string.h>
#include <ck/io.h>
#include <ck/socket.h>
#include <ck/ptr.h>
#include <pthread.h>
#include <ck/pair.h>
#include <ck/future.h>

// #define IPC_USE_SHM


namespace ck {

  namespace ipc {


    // using 32bits means the ``header'' of each sync message is 8 bytes total.
    // This size is *ONLY* worse than a 64bit nonce if you send 4,294,967,295
    // messages without receiving them on the peer connection. This would be
    // insane, and I'm not sure the rest of the system would even be able to
    // handle it. Seeing as the overhead of each message in the kernel is AT
    // LEAST 64 bytes... you need alot of ram
    using nonce_t = uint32_t;

    struct raw_msg {
      void* data;
      size_t sz;
    };

    class encoder;
    class decoder;



    template <typename T>
    bool encode(ck::ipc::encoder&, T&) {
      panic("base ck::ipc::encode instantiated\n");
      return false;
    }




    class encoder {
      //   encoder(void* buf, size_t sz) : buf(buf), sz(sz) {}
      uint8_t* m_buf;
      size_t& m_size;


     public:
      inline encoder(void* buf, size_t& size_out) : m_buf((uint8_t*)buf), m_size(size_out) {}
      template <typename T>
      encoder& operator<<(T& v) {
        ck::ipc::encode(*this, v);
        return *this;
      }

      template <typename T>
      encoder& operator<<(T v) {
        ck::ipc::encode(*this, v);
        return *this;
      }

      template <typename T>
      T& next(void) {
        auto cursz = m_size;

        m_size += sizeof(T);
        return *(T*)&m_buf[cursz];
      }


      void write(const void* buf, size_t sz) {
        memcpy(m_buf + m_size, buf, sz);
        m_size += sz;
      }

      void* data(void) { return (void*)m_buf; }
      size_t size(void) { return m_size; }
    };




#define SIMPLE_ENCODER(T)                         \
  template <>                                     \
  inline bool encode(ck::ipc::encoder& e, T& v) { \
    e.next<T>() = v;                              \
    return true;                                  \
  }

    SIMPLE_ENCODER(uint8_t);
    SIMPLE_ENCODER(int8_t);

    SIMPLE_ENCODER(uint16_t);
    SIMPLE_ENCODER(int16_t);

    SIMPLE_ENCODER(uint32_t);
    SIMPLE_ENCODER(int32_t);

    SIMPLE_ENCODER(uint64_t);
    SIMPLE_ENCODER(int64_t);

    SIMPLE_ENCODER(size_t);

    // no string or vector will be larger than 4gb, so we just use uint32_t to save 4 bytes :)

    template <>
    inline bool encode(ck::ipc::encoder& e, ck::string& v) {
      e << (uint32_t)v.size();
      e.write(v.get(), v.size());
      return true;
    }


    template <typename T>
    inline bool encode(ck::ipc::encoder& e, ck::vec<T>& v) {
      e << (uint32_t)v.size();
      for (auto& c : v) {
        ck::ipc::encode(e, c);
      }
      return true;
    }


#undef SIMPLE_ENCODER



    class decoder {
      void* buf;
      size_t sz;
      off_t head = 0;

     public:
      decoder(void* buf, size_t sz) : buf(buf), sz(sz) {}

      template <typename T>
      T& next(void) {
        if (head >= sz) {
          printf("head: %d, sz: %d\n", head, sz);
        }
        assert(head < sz);
        auto curhead = head;
        head += sizeof(T);

        return *(T*)((unsigned char*)buf + curhead);
      }

      template <typename T>
      T read() {
        return next<T>();
      }
    };


    // TODO:
    template <typename T>
    bool decode(ck::ipc::decoder&, T&) {
      panic("base ck::ipc::decode instantiated\n");
      return false;
    }

    template <typename T>
    T decode(ck::ipc::decoder& d) {
      T val;
      ck::ipc::decode(d, val);
      return val;
    }


#define SIMPLE_DECODER(T)                         \
  template <>                                     \
  inline bool decode(ck::ipc::decoder& d, T& v) { \
    v = d.next<T>();                              \
    return true;                                  \
  }

    SIMPLE_DECODER(uint8_t);
    SIMPLE_DECODER(int8_t);

    SIMPLE_DECODER(uint16_t);
    SIMPLE_DECODER(int16_t);

    SIMPLE_DECODER(uint32_t);
    SIMPLE_DECODER(int32_t);

    SIMPLE_DECODER(uint64_t);
    SIMPLE_DECODER(int64_t);

    SIMPLE_DECODER(size_t);

    // no string or vector will be larger than 4gb, so we just use uint32_t to save 4 bytes :)

    template <>
    inline bool decode(ck::ipc::decoder& d, ck::string& v) {
      auto sz = d.read<uint32_t>();
      for (int i = 0; i < sz; i++) {
        v.push(d.read<uint8_t>());
      }
      return true;
    }

    template <typename T>
    inline bool decode(ck::ipc::decoder& d, ck::vec<T>& v) {
      auto sz = d.read<uint32_t>();
      for (int i = 0; i < sz; i++) {
        v.push(d.read<T>());
      }
      return true;
    }

#undef SIMPLE_DECODER




    namespace impl {

      class socket_connection : public ck::refcounted<socket_connection> {
       public:
        socket_connection(ck::ref<ck::ipcsocket> s, const char* ns, bool is_server);
        virtual ~socket_connection(void) {
          if (!closed()) close();
          // wait on the recv fiber to finish... We really do not want to leak memory
          m_recv_fiber_join.await();
        }

        // a closed socket connection is just one without a socket :^)
        void close(void) {
          m_closed = true;
          // go through all 'sync' waiters and resolve their futures with NULL
          for (auto& w : sync_wait_futures)
            w.value.resolve({nullptr, 0});
          // THIS IS VERY HACKY
          m_sock->fire_on_read();
          if (on_close) on_close();
        }
        bool closed(void) { return m_closed; }

        ck::func<void()> on_close;

       protected:
        const char* side(void) { return is_server ? "server" : "client"; }
        // the encoder writes directly to the send_queue, finish-send just hits the doorbell
        ipc::encoder begin_send(void);
        void finish_send();
        void dispatch(void* data, size_t len);
        virtual void dispatch_received_message(void* data, size_t len) = 0;

        void handshake(void);
        // wait for a message where the first uint32_t is the needle.
        // Return a copy of the message when found, blocking forever
        // if no message is found. Any other messages are placed
        // in the stored_messages vector that needs to be dispatched
        // via dispatch_stored
        ck::ipc::raw_msg sync_wait(uint32_t msg_type, ck::ipc::nonce_t nonce);

        ck::vec<ck::ipc::raw_msg> stored_messages;



        struct shared_msg_data {
          size_t size;
          uint8_t data[];
        };

        struct shared_msg_queue {
          // we can use a mutex here because we know it's only an integer, nothing fancy
          pthread_mutex_t lock;
          ck::atom<int> count;
          struct shared_msg_data data[];
        };




        struct msg_handshake {
          size_t total_shared_msg_queue_size;
          char shm_name[32];
        };


        template <typename T>
        T recv_sync(void) {
          T out;
          m_sock->recv((void*)&out, sizeof(T), 0);
          return out;
        }


        ck::ref<ck::ipcsocket> m_sock;
        const char* ns;
        bool is_server;

        // each side gets 32kb of shared memory
        size_t total_shared_msg_queue_size = 64 * 4096;
        void* shared_msg_queue = NULL;

        // server recvs from position 0, sends to position 1
        // client recvs from position 1, sends to position 0
        struct shared_msg_queue* recv_queue = NULL;
        struct shared_msg_queue* send_queue = NULL;


#ifdef IPC_USE_SHM
        ck::string shm_name;
#else
        static constexpr size_t max_message_size = 32 * 1024;
        void* msg_buffer = NULL;
        size_t msg_size = 0;
#endif

        nonce_t get_nonce(void) {
          auto n = next_nonce++;
          return n;
        }
        nonce_t next_nonce = 0;


        ck::map<nonce_t, ck::future<raw_msg>> sync_wait_futures;

        ck::future<void> m_recv_fiber_join;

       private:
        ck::mutex m_lock;
        bool m_closed = false;
        inline ck::scoped_lock lock(void) { return ck::scoped_lock(m_lock); }
      };

    }  // namespace impl

    template <typename Connection>
    class server : public ck::refcounted<server<Connection>> {
     public:
      server(void) = default;
      virtual ~server(void) = default;


      // start the server listening on a given path
      void listen(ck::string listen_path) {
        assert(!m_server.listening());
        m_server.listen(listen_path, [this] {
          // printf("SERVER GOT A CONNECTION!\n");

          ck::ipcsocket* sock = m_server.accept();

          this->handle_connection(ck::make_ref<Connection>(sock));
        });
        m_listen_path = listen_path;
      }

     private:
      virtual void on_connection(ck::ref<Connection> c) {}
      virtual void on_disconnection(ck::ref<Connection> c) {}

      void handle_connection(ck::ref<Connection> s) {
        auto id = m_next_id++;
        Connection* sp = s.get();

        on_connection(s);

        if (sp->closed()) return;
        sp->on_close = [this, sp, id] {
          on_disconnection(sp);
          // printf("CLOSED!\n");
          m_connections.remove(id);
        };

        m_connections[id] = s;
      }



      long m_next_id = 0;
      ck::map<long, ck::ref<Connection>> m_connections;
      ck::string m_listen_path;
      ck::ipcsocket m_server;
    };



    template <typename Connection>
    ck::ref<Connection> connect(ck::string path) {
      // construct the ipc socket for the client connection to the server
      ck::ref<ck::ipcsocket> sock = ck::make_ref<ck::ipcsocket>();
      int res = sock->connect(path);
      if (!sock->connected()) {
        return nullptr;
      }

      return ck::make_ref<Connection>(sock);
    }


  }  // namespace ipc
}  // namespace ck