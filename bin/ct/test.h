/**
 *  Generated by ipcc.py. Any changes will be overwritten
 *
 * Namespace: test
 *
 * Client messages:
 *
 *   map (
 *     ck::vec<uint32_t> vec
 *   )
 *
 *   test (
 *     int x
 *   )
 *
 *
 * Server messages:
 *
 *   compute (
 *     uint32_t val
 *   )
 *
 */
#pragma once
#include <ck/ipc.h>
#include <ck/option.h>
#include <stdint.h>
#include <unistd.h>


namespace test {
  // An enumerated identifier for each kind of message
  enum class Message : uint32_t {
    CLIENT_MAP,
    CLIENT_MAP_RESPONSE,
    CLIENT_TEST,
    CLIENT_TEST_RESPONSE,
    SERVER_COMPUTE,
    SERVER_COMPUTE_RESPONSE
  };
  // return value for map sent from client
  struct client_map_response {
    ck::vec<uint32_t> vec;
  };
  // return value for test sent from client
  struct client_test_response {
    int x;
  };
  // return value for compute sent from client
  struct server_compute_response {
    uint32_t result;
  };
};  // namespace test




namespace test {
  class client_connection_stub : public ck::ipc::impl::socket_connection {
   public:
    client_connection_stub(ck::ref<ck::ipcsocket> s) : ck::ipc::impl::socket_connection(s, "test", false) {}
    virtual ~client_connection_stub(void) {}

    // methods to send messages
    inline ck::option<struct client_map_response> map(ck::vec<uint32_t> vec) {
      ck::ipc::encoder __e = begin_send();
      __e << (uint32_t)test::Message::CLIENT_MAP;
      ck::ipc::nonce_t __nonce = get_nonce();
      __e << (ck::ipc::nonce_t)__nonce;
      ck::ipc::encode(__e, vec);
      this->finish_send();
      // wait for the return value from the server :^)
      struct client_map_response res;
      auto [data, len] = sync_wait((uint32_t)test::Message::CLIENT_MAP_RESPONSE, __nonce);
      if (data == NULL && len == 0) return None;
      ck::ipc::decoder __decoder(data, len);
      auto __response_type = (test::Message)ck::ipc::decode<uint32_t>(__decoder);
      auto __response_nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
      assert(__response_type == test::Message::CLIENT_MAP_RESPONSE);
      assert(__response_nonce == __nonce);
      ck::ipc::decode(__decoder, res.vec);
      return res;
    }

    inline ck::option<struct client_test_response> test(int x) {
      ck::ipc::encoder __e = begin_send();
      __e << (uint32_t)test::Message::CLIENT_TEST;
      ck::ipc::nonce_t __nonce = get_nonce();
      __e << (ck::ipc::nonce_t)__nonce;
      ck::ipc::encode(__e, x);
      this->finish_send();
      // wait for the return value from the server :^)
      struct client_test_response res;
      auto [data, len] = sync_wait((uint32_t)test::Message::CLIENT_TEST_RESPONSE, __nonce);
      if (data == NULL && len == 0) return None;
      ck::ipc::decoder __decoder(data, len);
      auto __response_type = (test::Message)ck::ipc::decode<uint32_t>(__decoder);
      auto __response_nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
      assert(__response_type == test::Message::CLIENT_TEST_RESPONSE);
      assert(__response_nonce == __nonce);
      ck::ipc::decode(__decoder, res.x);
      return res;
    }


    // handle these in your subclass
    virtual ck::option<struct server_compute_response> on_compute(uint32_t val) {
      fprintf(stderr, "Unimplemneted IPC handler \"on_compute\" on test client\n");
      return {};
    }


   protected:
    // ^ck::ipc::impl::client_connection
    virtual inline void dispatch_received_message(void *data, size_t len) {
      ck::ipc::decoder __decoder(data, len);
      test::Message msg_type = (test::Message)ck::ipc::decode<uint32_t>(__decoder);
      switch (msg_type) {
        case test::Message::SERVER_COMPUTE: {
          auto __nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
          uint32_t val;
          ck::ipc::decode(__decoder, val);
          auto __res = on_compute(val);
          auto __out = __res.unwrap();
          // now to send the response back
          ck::ipc::encoder __e = begin_send();
          __e << (uint32_t)test::Message::SERVER_COMPUTE_RESPONSE;
          __e << (ck::ipc::nonce_t)__nonce;
          ck::ipc::encode(__e, __out.result);
          this->finish_send();
          break;
        }
        default:
          panic("client: Unhandled message type %d", msg_type);
          break;
      }
    }
  };
}  // namespace test


namespace test {
  class server_connection_stub : public ck::ipc::impl::socket_connection {
   public:
    server_connection_stub(ck::ref<ck::ipcsocket> s) : ck::ipc::impl::socket_connection(s, "test", true) {}
    virtual ~server_connection_stub(void) {}

    // methods to send messages
    inline ck::option<struct server_compute_response> compute(uint32_t val) {
      ck::ipc::encoder __e = begin_send();
      __e << (uint32_t)test::Message::SERVER_COMPUTE;
      ck::ipc::nonce_t __nonce = get_nonce();
      __e << (ck::ipc::nonce_t)__nonce;
      ck::ipc::encode(__e, val);
      this->finish_send();
      // wait for the return value from the server :^)
      struct server_compute_response res;
      auto [data, len] = sync_wait((uint32_t)test::Message::SERVER_COMPUTE_RESPONSE, __nonce);
      if (data == NULL && len == 0) return None;
      ck::ipc::decoder __decoder(data, len);
      auto __response_type = (test::Message)ck::ipc::decode<uint32_t>(__decoder);
      auto __response_nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
      assert(__response_type == test::Message::SERVER_COMPUTE_RESPONSE);
      assert(__response_nonce == __nonce);
      ck::ipc::decode(__decoder, res.result);
      return res;
    }


    // handle these in your subclass
    virtual ck::option<struct client_map_response> on_map(ck::vec<uint32_t> vec) {
      fprintf(stderr, "Unimplemneted IPC handler \"on_map\" on test server\n");
      return {};
    }
    virtual ck::option<struct client_test_response> on_test(int x) {
      fprintf(stderr, "Unimplemneted IPC handler \"on_test\" on test server\n");
      return {};
    }


   protected:
    // ^ck::ipc::impl::server_connection
    virtual inline void dispatch_received_message(void *data, size_t len) {
      ck::ipc::decoder __decoder(data, len);
      test::Message msg_type = (test::Message)ck::ipc::decode<uint32_t>(__decoder);
      switch (msg_type) {
        case test::Message::CLIENT_MAP: {
          auto __nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
          ck::vec<uint32_t> vec;
          ck::ipc::decode(__decoder, vec);
          auto __res = on_map(vec);
          auto __out = __res.unwrap();
          // now to send the response back
          ck::ipc::encoder __e = begin_send();
          __e << (uint32_t)test::Message::CLIENT_MAP_RESPONSE;
          __e << (ck::ipc::nonce_t)__nonce;
          ck::ipc::encode(__e, __out.vec);
          this->finish_send();
          break;
        }
        case test::Message::CLIENT_TEST: {
          auto __nonce = ck::ipc::decode<ck::ipc::nonce_t>(__decoder);
          int x;
          ck::ipc::decode(__decoder, x);
          auto __res = on_test(x);
          auto __out = __res.unwrap();
          // now to send the response back
          ck::ipc::encoder __e = begin_send();
          __e << (uint32_t)test::Message::CLIENT_TEST_RESPONSE;
          __e << (ck::ipc::nonce_t)__nonce;
          ck::ipc::encode(__e, __out.x);
          this->finish_send();
          break;
        }
        default:
          panic("server: Unhandled message type %d", msg_type);
          break;
      }
    }
  };
}  // namespace test
