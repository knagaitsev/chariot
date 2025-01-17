#define _CHARIOT_SRC

#include "sys/wait.h"
#include <chariot.h>
#include <chariot/ucontext.h>
#include <ck/command.h>
#include <ck/func.h>
#include <ck/io.h>
#include <ck/map.h>
#include <ck/parser.h>
#include <ck/ptr.h>
#include <ck/string.h>
#include <ck/vec.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysbind.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define C_RED "\x1b[31m"
#define C_GREEN "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_BLUE "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN "\x1b[36m"
#define C_RESET "\x1b[0m"
#define C_GRAY "\x1b[90m"


#include <linenoise.h>
#include <tree_sitter/api.h>
#include <tree_sitter/parser.h>
#include <wctype.h>


namespace {

  enum TokenType {
    HEREDOC_START,
    SIMPLE_HEREDOC_BODY,
    HEREDOC_BODY_BEGINNING,
    HEREDOC_BODY_MIDDLE,
    HEREDOC_BODY_END,
    FILE_DESCRIPTOR,
    EMPTY_VALUE,
    CONCAT,
    VARIABLE_NAME,
    REGEX,
    CLOSING_BRACE,
    CLOSING_BRACKET,
    HEREDOC_ARROW,
    HEREDOC_ARROW_DASH,
    NEWLINE,
  };

  struct Scanner {
    void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

    void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

    unsigned serialize(char *buffer) {
      if (heredoc_delimiter.size() + 3 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) return 0;
      buffer[0] = heredoc_is_raw;
      buffer[1] = started_heredoc;
      buffer[2] = heredoc_allows_indent;
      // TODO:
      // heredoc_delimiter.copy(&buffer[3], heredoc_delimiter.size());
      return heredoc_delimiter.size() + 3;
    }

    void deserialize(const char *buffer, unsigned length) {
      if (length == 0) {
        heredoc_is_raw = false;
        started_heredoc = false;
        heredoc_allows_indent = false;
        heredoc_delimiter.clear();
      } else {
        heredoc_is_raw = buffer[0];
        started_heredoc = buffer[1];
        heredoc_allows_indent = buffer[2];
        // heredoc_delimiter.assign(&buffer[3], &buffer[length]);
      }
    }

    bool scan_heredoc_start(TSLexer *lexer) {
      while (iswspace(lexer->lookahead))
        skip(lexer);

      lexer->result_symbol = HEREDOC_START;
      heredoc_is_raw = lexer->lookahead == '\'';
      started_heredoc = false;
      heredoc_delimiter.clear();

      if (lexer->lookahead == '\\') {
        advance(lexer);
      }

      int32_t quote = 0;
      if (heredoc_is_raw || lexer->lookahead == '"') {
        quote = lexer->lookahead;
        advance(lexer);
      }

      while (iswalpha(lexer->lookahead) || (quote != 0 && iswspace(lexer->lookahead))) {
        heredoc_delimiter += lexer->lookahead;
        advance(lexer);
      }

      if (lexer->lookahead == quote) {
        advance(lexer);
      }

      return !(heredoc_delimiter.size() == 0);
    }

    bool scan_heredoc_end_identifier(TSLexer *lexer) {
      current_leading_word.clear();
      // Scan the first 'n' characters on this line, to see if they match the heredoc delimiter
      while (lexer->lookahead != '\0' && lexer->lookahead != '\n' && current_leading_word.size() < heredoc_delimiter.size()) {
        current_leading_word += lexer->lookahead;
        advance(lexer);
      }
      return current_leading_word == heredoc_delimiter;
    }

    bool scan_heredoc_content(TSLexer *lexer, TokenType middle_type, TokenType end_type) {
      bool did_advance = false;

      for (;;) {
        switch (lexer->lookahead) {
          case '\0': {
            if (did_advance) {
              heredoc_is_raw = false;
              started_heredoc = false;
              heredoc_allows_indent = false;
              heredoc_delimiter.clear();
              lexer->result_symbol = end_type;
              return true;
            } else {
              return false;
            }
          }

          case '\\': {
            did_advance = true;
            advance(lexer);
            advance(lexer);
            break;
          }

          case '$': {
            if (heredoc_is_raw) {
              did_advance = true;
              advance(lexer);
              break;
            } else if (did_advance) {
              lexer->result_symbol = middle_type;
              started_heredoc = true;
              return true;
            } else {
              return false;
            }
          }

          case '\n': {
            did_advance = true;
            advance(lexer);
            if (heredoc_allows_indent) {
              while (iswspace(lexer->lookahead)) {
                advance(lexer);
              }
            }
            if (scan_heredoc_end_identifier(lexer)) {
              heredoc_is_raw = false;
              started_heredoc = false;
              heredoc_allows_indent = false;
              heredoc_delimiter.clear();
              lexer->result_symbol = end_type;
              return true;
            }
            break;
          }

          default: {
            did_advance = true;
            advance(lexer);
            break;
          }
        }
      }
    }

    bool scan(TSLexer *lexer, const bool *valid_symbols) {
      if (valid_symbols[CONCAT]) {
        if (!(lexer->lookahead == 0 || iswspace(lexer->lookahead) || lexer->lookahead == '\\' || lexer->lookahead == '>' ||
                lexer->lookahead == '<' || lexer->lookahead == ')' || lexer->lookahead == '(' || lexer->lookahead == ';' ||
                lexer->lookahead == '&' || lexer->lookahead == '|' || lexer->lookahead == '`' || lexer->lookahead == '#' ||
                (lexer->lookahead == '}' && valid_symbols[CLOSING_BRACE]) || (lexer->lookahead == ']' && valid_symbols[CLOSING_BRACKET]))) {
          lexer->result_symbol = CONCAT;
          return true;
        }
      }

      if (valid_symbols[EMPTY_VALUE]) {
        if (iswspace(lexer->lookahead)) {
          lexer->result_symbol = EMPTY_VALUE;
          return true;
        }
      }

      if (valid_symbols[HEREDOC_BODY_BEGINNING] && !heredoc_delimiter.empty() && !started_heredoc) {
        return scan_heredoc_content(lexer, HEREDOC_BODY_BEGINNING, SIMPLE_HEREDOC_BODY);
      }

      if (valid_symbols[HEREDOC_BODY_MIDDLE] && !heredoc_delimiter.empty() && started_heredoc) {
        return scan_heredoc_content(lexer, HEREDOC_BODY_MIDDLE, HEREDOC_BODY_END);
      }

      if (valid_symbols[HEREDOC_START]) {
        return scan_heredoc_start(lexer);
      }

      if (valid_symbols[VARIABLE_NAME] || valid_symbols[FILE_DESCRIPTOR] || valid_symbols[HEREDOC_ARROW]) {
        for (;;) {
          if (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
              (lexer->lookahead == '\n' && !valid_symbols[NEWLINE])) {
            skip(lexer);
          } else if (lexer->lookahead == '\\') {
            skip(lexer);
            if (lexer->lookahead == '\r') {
              skip(lexer);
            }
            if (lexer->lookahead == '\n') {
              skip(lexer);
            } else {
              return false;
            }
          } else {
            break;
          }
        }

        if (valid_symbols[HEREDOC_ARROW] && lexer->lookahead == '<') {
          advance(lexer);
          if (lexer->lookahead == '<') {
            advance(lexer);
            if (lexer->lookahead == '-') {
              advance(lexer);
              heredoc_allows_indent = true;
              lexer->result_symbol = HEREDOC_ARROW_DASH;
            } else if (lexer->lookahead == '<') {
              return false;
            } else {
              heredoc_allows_indent = false;
              lexer->result_symbol = HEREDOC_ARROW;
            }
            return true;
          }
          return false;
        }

        bool is_number = true;
        if (iswdigit(lexer->lookahead)) {
          advance(lexer);
        } else if (iswalpha(lexer->lookahead) || lexer->lookahead == '_') {
          is_number = false;
          advance(lexer);
        } else {
          return false;
        }

        for (;;) {
          if (iswdigit(lexer->lookahead)) {
            advance(lexer);
          } else if (iswalpha(lexer->lookahead) || lexer->lookahead == '_') {
            is_number = false;
            advance(lexer);
          } else {
            break;
          }
        }

        if (is_number && valid_symbols[FILE_DESCRIPTOR] && (lexer->lookahead == '>' || lexer->lookahead == '<')) {
          lexer->result_symbol = FILE_DESCRIPTOR;
          return true;
        }

        if (valid_symbols[VARIABLE_NAME]) {
          if (lexer->lookahead == '+') {
            lexer->mark_end(lexer);
            advance(lexer);
            if (lexer->lookahead == '=') {
              lexer->result_symbol = VARIABLE_NAME;
              return true;
            } else {
              return false;
            }
          } else if (lexer->lookahead == '=' || lexer->lookahead == '[') {
            lexer->result_symbol = VARIABLE_NAME;
            return true;
          }
        }

        return false;
      }

      if (valid_symbols[REGEX]) {
        while (iswspace(lexer->lookahead))
          skip(lexer);

        if (lexer->lookahead != '"' && lexer->lookahead != '\'' && lexer->lookahead != '$') {
          struct State {
            bool done;
            uint32_t paren_depth;
            uint32_t bracket_depth;
            uint32_t brace_depth;
          };

          lexer->mark_end(lexer);

          State state = {false, 0, 0, 0};
          while (!state.done) {
            switch (lexer->lookahead) {
              case '\0':
                return false;
              case '(':
                state.paren_depth++;
                break;
              case '[':
                state.bracket_depth++;
                break;
              case '{':
                state.brace_depth++;
                break;
              case ')':
                if (state.paren_depth == 0) state.done = true;
                state.paren_depth--;
                break;
              case ']':
                if (state.bracket_depth == 0) state.done = true;
                state.bracket_depth--;
                break;
              case '}':
                if (state.brace_depth == 0) state.done = true;
                state.brace_depth--;
                break;
            }

            if (!state.done) {
              bool was_space = iswspace(lexer->lookahead);
              advance(lexer);
              if (!was_space) lexer->mark_end(lexer);
            }
          }

          lexer->result_symbol = REGEX;
          return true;
        }
      }

      return false;
    }

    ck::string heredoc_delimiter;
    bool heredoc_is_raw;
    bool started_heredoc;
    bool heredoc_allows_indent;
    ck::string current_leading_word;
  };

}  // namespace

extern "C" {

void *tree_sitter_shell_external_scanner_create() { return new Scanner(); }

bool tree_sitter_shell_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

unsigned tree_sitter_shell_external_scanner_serialize(void *payload, char *state) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->serialize(state);
}

void tree_sitter_shell_external_scanner_deserialize(void *payload, const char *state, unsigned length) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  scanner->deserialize(state, length);
}

void tree_sitter_shell_external_scanner_destroy(void *payload) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  delete scanner;
}
TSLanguage *tree_sitter_shell();
}

size_t current_us() { return syscall(SYS_gettime_microsecond); }

extern char **environ;


#define MAX_ARGS 255

void reset_pgid() {
  pid_t pgid = getpgid(0);
  tcsetpgrp(0, pgid);
}


pid_t shell_fork() {
  pid_t pid = fork();

  if (pid == 0) {
    // create a new group for the process
    int res = setpgid(0, 0);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
  }
  return pid;
}


#define RUN_NOFORK 1

#define TOK_ARG 1
#define TOK_PIPE 2


pid_t fg_pid = -1;

class Expression {
 public:
  Expression(const ck::string &source, TSNode node) : m_source(source), m_node(node) { m_value = node_str(node); }

  virtual ~Expression(void) = default;
  virtual const char *type(void) const { return ts_node_type(m_node); }

  virtual ck::string format(void) { return "<expr>"; }
  const ck::string &value(void) const { return m_value; }
  static ck::box<Expression> convert(const ck::string &source, TSNode node);
  inline auto len(void) const { return m_children.size(); }
  inline Expression *child(off_t ind) const { return m_children[ind].get(); }
  void dump(int depth = 0);

  inline bool is(const char *type) const { return strcmp(type, this->type()) == 0; }

  virtual void exec(void) {
    fprintf(stderr, "error: unable to evaluate %s yet.\n", type());
    exit(-1);
  }


 protected:
  // Parse the node before adding it to the expression list
  virtual void parse(Expression &node) {}

  inline ck::string node_str(TSNode &node) {
    ck::string str;
    auto start = ts_node_start_byte(node);
    auto end = ts_node_end_byte(node);
    for (auto i = start; i < end; i++)
      str += m_source[i];
    return str;
  }

 private:
  ck::vec<ck::box<Expression>> m_children;
  ck::string m_value;
  TSNode m_node;
  const ck::string &m_source;

  void add_child(ck::box<Expression> &&child) {
    parse(*child);
    m_children.push(move(child));
  }
};

#define EXPR_TYPE(T)            \
  using Expression::Expression; \
  const char *type(void) const override { return "" #T; }




class Program : public Expression {
 public:
  EXPR_TYPE(Program);

  void exec(void) override {
    for (int i = 0; i < len(); i++) {
      child(i)->exec();
    }
  }
};




class Command : public Expression {
 public:
  EXPR_TYPE(Command);

  void parse(Expression &expr) override {
    if (expr.is("command_name")) {
      command = expr.value();
      return;
    }

    if (expr.is("word")) {
      args.push(expr.value());
      return;
    }

    if (expr.is("string") || expr.is("raw_string")) {
      // trim the "" from the outside
      // This is a really bad way to do this
      ck::string s = expr.value().get() + 1;
      s.pop();
      args.push(s);
      return;
    }
  }

  void exec(void) override {
    // printf("exec %s\n", format().get());
    if (command == "cd") {
      const char *path = nullptr;
      if (args.size() == 0) {
        uid_t uid = getuid();
        struct passwd *pwd = getpwuid(uid);
        // TODO: get user $HOME and go there instead
        path = pwd->pw_dir;
      } else {
        path = args[0].get();
      }
      int res = chdir(path);
      if (res != 0) {
        printf("cd: '%s' could not be entered\n", path);
      }
      return;
    }

    if (command == "exit") {
      exit(0);
    }


    ck::vec<const char *> argv;
    // push the command, then the arguments
    argv.push(command.get());
    for (auto &arg : args)
      argv.push(arg.get());
    // Null terminate the list
    argv.push(nullptr);


    pid_t pid = fork();
    /*
    if ((flags & RUN_NOFORK) == 0) {
      pid = fork();
    }
    */

    if (pid == 0) {
      setpgid(0, 0);  // TODO: is this right?

      execvpe(argv[0], argv.data(), (const char **)environ);
      int err = errno;  // grab errno now (curse you globals)

      const char *serr = strerror(err);
      if (errno == ENOENT) {
        serr = "command not found";
      }
      printf("%s: \x1b[31m%s\x1b[0m\n", argv[0], serr);
      exit(EXIT_FAILURE);
      exit(-1);
    }

    fg_pid = pid;

    int res = 0;
    /* wait for the subproc */
    do
      waitpid(pid, &res, 0);
    while (errno == EINTR);

    fg_pid = -1;

    if (WIFSIGNALED(res)) {
      printf("%s: \x1b[31mterminated with signal %d\x1b[0m\n", argv[0], WTERMSIG(res));
    } else if (WIFEXITED(res) && WEXITSTATUS(res) != 0) {
      printf("%s: \x1b[31mexited with code %d\x1b[0m\n", argv[0], WEXITSTATUS(res));
    }
  }
  ck::string format(void) override {
    ck::string res = command;
    for (auto &arg : args) {
      res.appendf(" \"%s\"", arg.get());
    }
    return res;
  }

  ck::string command;
  ck::vec<ck::string> args;
};




ck::box<Expression> Expression::convert(const ck::string &source, TSNode node) {
  if (ts_node_has_error(node)) {
    return nullptr;
  }
  ck::box<Expression> e;

  ck::string type = ts_node_type(node);
  if (!e && type == "program") e = ck::make_box<Program>(source, node);
  if (!e && type == "command") e = ck::make_box<Command>(source, node);
  if (!e) e = ck::make_box<Expression>(source, node);

  for (int i = 0; i < ts_node_named_child_count(node); i++) {
    // Ask the expression to parse the sub-nodes
    e->add_child(Expression::convert(source, (ts_node_named_child(node, i))));
  }
  return e;
}


void Expression::dump(int depth) {
  auto indent = [&] {
    for (int i = 0; i < depth; i++)
      printf("  ");
  };
  indent();
  printf("%s: \x1b[31m%s\x1b[0m \x1b[33m%s\x1b[0m\n", type(), value().get(), format().get());
  for (int i = 0; i < len(); i++) {
    child(i)->dump(depth + 1);
  }
}


void run_source(ck::string source) {
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_shell());
  TSTree *tree = ts_parser_parse_string(parser, NULL, source.get(), source.size());
  TSNode root_node = ts_tree_root_node(tree);
  auto program = Expression::convert(source, root_node);
  if (program) {
    // program->dump();
    program->exec();
  } else {
    printf("Syntax error\n");
  }
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}




char hostname[256];
char uname[128];
char cwd[255];

void sigint_handler(int sig, void *, void *uc) {
  if (fg_pid != -1) {
    kill(-fg_pid, sig);
  }
}

#include <ck/dir.h>

ck::vec<ck::string> possible_files(ck::string partial_path) {
  ck::vec<ck::string> files;
  return files;
}




int main(int argc, char **argv, char **envp) {
  int ch;
  const char *flags = "c:";
  while ((ch = getopt(argc, argv, flags)) != -1) {
    switch (ch) {
      case 'c': {
        run_source(optarg);
        exit(EXIT_SUCCESS);
        break;
      }
      case '?':
        puts("sh: invalid option\n");
        return -1;
    }
  }


  signal(SIGINT, (void (*)(int))sigint_handler);

  /* TODO: is this right? */
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigprocmask(SIG_UNBLOCK, &set, NULL);

  /* TODO: isatty() :) */
  reset_pgid();

  /* Read the hostname */
  int hn = open("/cfg/hostname", O_RDONLY);
  int n = read(hn, (void *)hostname, 256);
  if (n >= 0) {
    hostname[n] = 0;
    for (int i = n; i > 0; i--) {
      if (hostname[i] == '\n') hostname[i] = 0;
    }
  }
  close(hn);


  uid_t uid = getuid();
  struct passwd *pwd = getpwuid(uid);

  ck::string uname = pwd->pw_name;

  setenv("USER", pwd->pw_name, 1);
  setenv("SHELL", pwd->pw_shell, 1);
  setenv("HOME", pwd->pw_dir, 1);

  /* Set the completion callback. This will be called every time the
   * user uses the <tab> key. */
  linenoiseSetMultiLine(1);

  struct termios tios;
  while (1) {
    tcgetattr(0, &tios);
    syscall(SYS_getcwd, cwd, 255);
    setenv("CWD", (const char *)cwd, 1);


    const char *disp_cwd = (const char *)cwd;
    if (strcmp((const char *)cwd, pwd->pw_dir) == 0) {
      disp_cwd = "~";
    }

    auto prompt =
        ck::string::format("[\x1b[33m%s\x1b[0m@\x1b[34m%s \x1b[35m%s\x1b[0m]%c ", uname.get(), hostname, disp_cwd, uid == 0 ? '#' : '$');


    const char *raw_line = linenoise(prompt.get());
    if (raw_line != NULL) {
      ck::string line = raw_line;
      if (line.len() == 0) continue;

      // run_line(line);
      run_source(line);
      linenoiseHistoryAdd(raw_line);
      linenoiseHistorySave("/tmp/.readline");
      linenoiseFree((void *)raw_line);
      tcsetattr(0, TCSANOW, &tios);
      reset_pgid();
    }
  }

  return 0;
}
