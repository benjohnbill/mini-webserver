# CONVENTION.md

Rules that people and agents both follow in this repository. This file is a proposal from the `CJG` branch. The section "Team rules" is empty until the team meeting decides it.

## Team rules

To be decided at the team meeting. Until then, the rules of `README.md` (section "작업 방식") apply: each member works on a personal branch, and `main` takes changes only through a PR with a merge commit.

## Code

The language is C11 with POSIX.1-2008 (`-std=c11 -D_POSIX_C_SOURCE=200809L`). Use only the C standard library and POSIX APIs. Exception on `CJG`: the Reverse Proxy links OpenSSL (`docs/adr/openssl-for-the-reverse-proxy.md`). Do not use a complete HTTP or event loop library (libevent, libuv, nginx, Apache, mongoose).

### Format

`.clang-format` sets the shape: Linux style with a 4-space indent, braces always, and 80 columns. It is the same file as the Pintos team repositories and mini-redis. `.clang-tidy` checks the form of names (snake_case, constants in upper case) and their length. The rules under "Names" and "Functions" are checked in review, because the tools cannot check them.

```bash
make format   # fixes the shape
make lint     # prints violations and fails if there is one
```

Use clang-format 21.1.8 and clang-tidy 21.1.6. Different versions format the same lines differently.

### Names

1. **A variable name says the role of what it holds.** Exceptions: loop counters `i`, `j`, `k`, a parameter `n`, and a file descriptor `fd`.
2. **A function that another file calls has the name `module_verb`** (`http_parse_request`, `mime_find_type`). A `static` function starts with the object that it works on (`queue_push`, `connection_close`).
3. **One verb has one meaning.**

   | Verb | Meaning |
   |---|---|
   | `create` and `destroy` | Allocate and make. Free. Use them as a pair |
   | `init` | Set memory that already exists. It does not allocate |
   | `find` | Look for a thing. Return `NULL` or `false` if it is not there |
   | `parse` | Read bytes into a struct. Return a status code, or false, on failure |
   | `resolve` | Turn a name into the thing that it names. Return 0 or a status code |
   | `measure` | Return a length |
   | `get` | Return a value. Change nothing |
   | `send` | Write bytes to a socket |
   | `is_` and `has_` | Ask a question and return `bool` |
   | `format` | Write the text form of a struct into a buffer that the caller gives |
   | `take` | Use up one unit of a limited supply. Return 0 if it was there |
   | `decide` | Turn signals into one choice from an enum |
   | `fetch` | Send a request and read the full reply |
   | `add` | Put one more item into a struct that exists. Return false, and change nothing, if it does not fit |

4. **One concept has one name in all files.** When you add a concept, add a row.

   | Concept | Name | Do not use |
   |---|---|---|
   | A parsed request | `struct request *request` | `req`, `r` |
   | A Response plan | `struct response_plan *plan` | `resp`, `res` |
   | A Route | `struct route *route` | `rt` |
   | A connection socket | `conn_fd` | `cfd`, `client` |
   | The listen socket | `listen_fd` | `sfd`, `server_fd` |
   | An HTTP status code | `status` | `code`, `sc` |
   | The run-time settings | `struct config *config` | `cfg`, `opts` |
   | The Reverse Proxy cache | `struct cache *cache` | `c`, `store` |
   | One stored reply in the cache | `struct cache_entry *entry` | `node`, `item` |
   | A parsed ClientHello | `struct client_hello *hello` | `ch`, `chello` |
   | The rate limit table | `struct rate_limit *limit` | `rl`, `limiter` |
   | The Origins of the Reverse Proxy | `struct origin_list *list` | `backends`, `upstreams` |
   | The block list (CIDR ranges) | `struct ip_list *block_list` | `list`, `ranges`, `blocklist` |
   | The JA4 table | `struct ja4_table *fingerprints` | `table`, `ja4s` |
   | The block table | `struct block_table *blocks` | `table`, `bans`, `jail` |
   | The Behavior counter table | `struct behavior *counters` | `table`, `stats`, `tracker` |
   | The Web Bot Auth keys | `struct bot_key_set *keys` | `bots`, `key_set`, `bot_keys` |

5. **Do not put `typedef` on a struct. Give a name to each number constant** (`HEADER_MAX`).

### Functions

1. **A function reads as a list of steps.** If one part with one purpose is longer than 3 lines, move it to a `static` function with a verb name.
2. **A file reads from top to bottom.** Put the `static` declarations at the top, then the public functions, then the helpers in the order of their first call.
3. **Put a blank line before the last `return`** when 3 or more lines come before it with no blank line. An early `return` and a short function do not get one.

## Tests

- Unit tests are in `tests/test_<module>.c`. They call the public functions of the Shared core with strings, with no socket. Each module has one test executable. The test macros are in `tests/check.h`. We use no test framework.
- A test name is an English sentence that says the behavior. The list of edge cases is the list of test names.
- Write the expected value as a literal. Do not compute it with the function under test.
- The functional tests are in `tests/func.sh`. They use curl and `nc -C` against a running server.
- The Scenes are in `bench/scene.sh`. A Scene measures. It does not pass or fail.

## Docs

| Content | Place |
|---|---|
| How to build and run, and the file map | `README.md` (team file) |
| The design of one branch | `docs/spec-<initials>.md` |
| A decision that is hard to reverse | `docs/adr/` |
| A small reason | A comment on that line of code |
| An edge case | A test name |
| The result of one Scene run | `docs/measurements/` |
| A term | `GLOSSARY.md` |

### ADR

- Put an ADR in `docs/adr/<short-english-name>.md`. Do not number the file. Numbers from different branches collide at merge.
- The frontmatter has `status` and `branch`.
- The body is one table with two columns. The rows are Context, Decision, Reasons, Rejected options, and Consequences, in this order. Write the last two rows only when they help.
- An ADR must pass all three tests: it is hard to reverse, a reader without context asks "why?", and a real alternative existed.
- The Reasons row quotes what a person said. It does not hold a reason that an AI inferred.
- To change a decision, write a new ADR. Set the `status` of the old ADR to `superseded by <new file name>`.

### Language

All docs in this repository are in ASD-STE100 English: short sentences, active voice, one idea per sentence. A quote of a person stays in the language of the person.
