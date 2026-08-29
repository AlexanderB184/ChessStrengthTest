#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chess.h"

typedef enum bot_state_t {
  BOT_STARTING,
  BOT_IDLE,
  BOT_PONDERING,
  BOT_RUNNING,
  BOT_FINISHED,
  BOT_DEAD,
  BOT_CLOSED,
  BOT_ERROR,
} bot_state_t;

typedef struct bot_iface_t {
  // current game position
  chess_state_t game_state;

  // best move generated from previous execution
  move_t bestmove;

  // ponder token generated from previous execution
  move_t pondertoken;

  // BOT_STARTING:  bot is launched and once it has sent "uciok" its status will
  // be set to "BOT_IDLE"
  //
  // BOT_IDLE:      bot is launched and ready and is awaiting a command
  //
  // BOT_PONDERING: bot is pondering a move, can either be cancelled with
  // "stop".
  //                     "ponderhit" can be sent to set the bot to "BOT_RUNNING"
  //
  // BOT_RUNNING:   bot is running in normal mode, can be terminated early with
  // "stop".
  //
  // BOT_FINISHED:  bot has finished running and there is a "bestmove" available
  // to be taken.
  //                     Otherwise identical to "BOT_IDLE". Once bestmove has
  //                     been consumed it will return to "IDLE".
  //
  // BOT_DEAD:      bot has crashed unexpectedly and is awaiting a restart.
  //
  // BOT_CLOSED:    bot has been sent the "quit" command and has shut down.
  //
  // BOT_ERROR:     bot has attempted to make an illegal move. Error should be
  // treated as a forfeit
  atomic_int bot_state;

  FILE* writer;
  FILE* reader;

  pid_t pid;
  pthread_t msg_reader_handle;

  char* path;
  char* name;
} bot_iface_t;

int uci_is_whitespace(char c) { return c == ' ' || c == '\t'; }

int uci_is_newline(char c) { return c == '\n'; }

int uci_is_delim(char c) {
  return c == 0 || c == ' ' || c == '\t' || c == '\n';
}

int uci_strcmp(const char* lhs, const char* rhs) {
  if (lhs == NULL || rhs == NULL) {
    return 0;
  }
  while (!uci_is_delim(*lhs) && !uci_is_delim(*rhs)) {
    if (*lhs != *rhs) return 0;
    lhs++;
    rhs++;
  }
  return uci_is_delim(*lhs) && uci_is_delim(*rhs);
}

void uci_read_from_stdin(bot_iface_t* bot_iface, char* buffer, int buffer_size,
                         int* pos, int keep) {
  if (keep == 0 && buffer[0]) {
    keep = *pos;
  }
  *pos = *pos - keep;
  memmove(buffer, &buffer[keep], *pos);
  if (fgets(&buffer[*pos], buffer_size - *pos, bot_iface->reader) == NULL) {
    if (feof(bot_iface->reader)) {
      atomic_store(&bot_iface->bot_state, BOT_DEAD);
    } else {
      atomic_store(&bot_iface->bot_state, BOT_DEAD);
    }
  }
}

void uci_next_arg(bot_iface_t* bot_iface, char** arg, int* arg_len,
                  char* buffer, int buffer_size, int* pos) {
  do {
    while (buffer[*pos] && uci_is_whitespace(buffer[*pos])) {
      (*pos)++;
    }

    if (buffer[*pos] == 0) {
      uci_read_from_stdin(bot_iface, buffer, buffer_size, pos, *pos);
      continue;
    }
    break;
  } while (bot_iface->bot_state != BOT_DEAD);

  if (uci_is_newline(buffer[*pos])) {
    (*pos)++;
    *arg = NULL;
    *arg_len = 0;
    // printf("[reader] newline\n");
    return;
  }

  int arg_start = *pos;

  do {
    while (buffer[*pos] && !uci_is_whitespace(buffer[*pos]) &&
           !uci_is_newline(buffer[*pos])) {
      (*pos)++;
    }
    if (buffer[*pos] == 0) {
      uci_read_from_stdin(bot_iface, buffer, buffer_size, pos, arg_start);
      arg_start = 0;
      continue;
    }

    break;
  } while (bot_iface->bot_state != BOT_DEAD);

  *arg_len = *pos - arg_start;

  // printf("token %.*s\n", arglen, &buffer[arg_start]);
  *arg = &buffer[arg_start];
  // printf("[reader] \'%.*s\'\n", *arg_len, &buffer[arg_start]);
}

#define BUFFER_SIZE 1024

void* bot_msg_reader(void* arg) {
  bot_iface_t* bot_iface = (bot_iface_t*)arg;

  char buffer[BUFFER_SIZE];
  int pos = 0;
  char* msg_arg = NULL;
  int arg_len = 0;

  buffer[0] = 0;
  for (;;) {
    int no_cmd = 1;
    do {
      uci_next_arg(bot_iface, &msg_arg, &arg_len, buffer, BUFFER_SIZE, &pos);

      if (uci_strcmp(msg_arg, "uciok")) {
        no_cmd = 0;
        if (atomic_load(&bot_iface->bot_state) == BOT_STARTING) {
          atomic_store(&bot_iface->bot_state, BOT_IDLE);
        }
      } else if (uci_strcmp(msg_arg, "id")) {
        no_cmd = 0;
        uci_next_arg(bot_iface, &msg_arg, &arg_len, buffer, BUFFER_SIZE, &pos);
        if (uci_strcmp(msg_arg, "name")) {
          uci_next_arg(bot_iface, &msg_arg, &arg_len, buffer, BUFFER_SIZE,
                       &pos);
          if (msg_arg != NULL) {
            if (bot_iface->name) free(bot_iface->name);
            char* name = malloc(arg_len + 1);
            memcpy(name, msg_arg, arg_len);
            name[arg_len] = 0;
            bot_iface->name = name;
          }
        }
      } else if (uci_strcmp(msg_arg, "readyok")) {
        no_cmd = 0;
      } else if (uci_strcmp(msg_arg, "info")) {
        no_cmd = 0;
      } else if (uci_strcmp(msg_arg, "bestmove")) {
        uci_next_arg(bot_iface, &msg_arg, &arg_len, buffer, BUFFER_SIZE, &pos);
        if (msg_arg == NULL) {
          atomic_store(&bot_iface->bot_state, BOT_ERROR);
          break;
        }
        long r = read_long_algebraic_notation(
            msg_arg, arg_len, &bot_iface->game_state, &bot_iface->bestmove);
        if (r < 0 ||
            !is_pseudo_legal(&bot_iface->game_state, bot_iface->bestmove) ||
            !is_legal(&bot_iface->game_state, bot_iface->bestmove)) {
          // bot tried to make illegal move
          atomic_store(&bot_iface->bot_state, BOT_ERROR);
          break;
        }
        make_move(&bot_iface->game_state, bot_iface->bestmove);

        // r = read_long_algebraic_notation(
        //     msg_arg, arg_len, &bot_iface->game_state,
        //     &bot_iface->pondertoken);
        // if (r < 0 ||
        //     !is_pseudo_legal(&bot_iface->game_state, bot_iface->pondertoken)
        //     || !is_legal(&bot_iface->game_state, bot_iface->pondertoken)) {
        //   // bot ponder token is illegal move
        //   atomic_store(&bot_iface->bot_state, BOT_ERROR);
        //   break;
        // }

        atomic_store(&bot_iface->bot_state, BOT_FINISHED);

        no_cmd = 0;
      }
    } while (no_cmd);

    while (msg_arg != NULL) {
      uci_next_arg(bot_iface, &msg_arg, &arg_len, buffer, BUFFER_SIZE, &pos);
    }
  }
}
#undef BUFFER_SIZE

int send_msg(bot_iface_t* bot_iface, const char* msg, size_t msg_len) {
  fwrite(msg, 1, msg_len, bot_iface->writer);
  // fwrite(msg, 1, msg_len, stdout);
  fputc('\n', bot_iface->writer);
  // fputc('\n', stdout);
  fflush(bot_iface->writer);
  // fflush(stdout);
  return 0;
}

typedef struct search_config_t {
  bool can_ponder;
  // limit the search to only these responses
  move_t* search_moves;
  int search_moves_count;

  size_t time_limit_ms;
  size_t node_limit;
  size_t depth_limit;
  size_t mate_in_limit;

  size_t wtime;
  size_t btime;
  size_t winc;
  size_t binc;
} search_config_t;

int uci_send_ucinewgame(bot_iface_t* bot_iface) {
  return send_msg(bot_iface, "ucinewgame", 10);
}

int uci_send_position(bot_iface_t* bot_iface, chess_state_t* chess_state) {
  // unmake to last irreversible move or root, save fen, then store each move
  // after to move text
  char cmd[1024] = "position fen ";
  int idx = strlen(cmd);
  move_t move_stack[128];
  int ply_count = 0;

  while (chess_state->ply_counter >
             chess_state->ply_of_last_irreversible_move &&
         chess_state->ply_counter > 0) {
    move_stack[ply_count++] =
        chess_state->ply_stack[chess_state->ply_counter - 1].move;
    unmake_move(chess_state);
  }

  idx += save_position(chess_state, cmd + idx, sizeof(cmd) - idx);

  if (ply_count != 0) {
    cmd[idx++] = ' ';
    cmd[idx++] = 'm';
    cmd[idx++] = 'o';
    cmd[idx++] = 'v';
    cmd[idx++] = 'e';
    cmd[idx++] = 's';

    for (int i = ply_count - 1; i >= 0; i--) {
      cmd[idx++] = ' ';
      make_move(chess_state, move_stack[i]);
      idx += write_long_algebraic_notation(cmd + idx, sizeof(cmd) - idx,
                                           move_stack[i]);
    }
  }

  return send_msg(bot_iface, cmd, idx);
}

int uci_send_go(bot_iface_t* bot_iface, const search_config_t* config) {
  if (config == NULL) {
    config = &(search_config_t){.can_ponder = 0, .time_limit_ms = 100};
  }
  char msg_buffer[1024] = "";
  long bytes_written = 0;
  bytes_written += snprintf(msg_buffer + bytes_written,
                            sizeof(msg_buffer) - bytes_written, "go ");
  if (config->search_moves) {
  }
  if (config->can_ponder) {
    bytes_written += snprintf(msg_buffer + bytes_written,
                              sizeof(msg_buffer) - bytes_written, "ponder ");
  }
  if (config->btime != SIZE_MAX) {
    bytes_written +=
        snprintf(msg_buffer + bytes_written, sizeof(msg_buffer) - bytes_written,
                 "wtime %ld btime %ld winc %ld binc %ld ", config->wtime,
                 config->btime, config->winc, config->binc);
  }
  if (config->depth_limit) {
    bytes_written +=
        snprintf(msg_buffer + bytes_written, sizeof(msg_buffer) - bytes_written,
                 "depth %ld ", config->depth_limit);
  }
  if (config->node_limit) {
    bytes_written +=
        snprintf(msg_buffer + bytes_written, sizeof(msg_buffer) - bytes_written,
                 "nodes %ld ", config->node_limit);
  }
  if (config->time_limit_ms) {
    bytes_written +=
        snprintf(msg_buffer + bytes_written, sizeof(msg_buffer) - bytes_written,
                 "movetime %ld ", config->time_limit_ms);
  }
  if (config->mate_in_limit) {
    bytes_written +=
        snprintf(msg_buffer + bytes_written, sizeof(msg_buffer) - bytes_written,
                 "matein %ld ", config->mate_in_limit);
  }
  if (!config->depth_limit && !config->node_limit && !config->time_limit_ms &&
      !config->mate_in_limit) {
    bytes_written += snprintf(msg_buffer + bytes_written,
                              sizeof(msg_buffer) - bytes_written, "infinite ");
  }

  return send_msg(bot_iface, msg_buffer, bytes_written);
}

int init_bot(bot_iface_t* bot_iface, const char* path) {
  int in_fd[2];
  int out_fd[2];

  if (pipe(in_fd) == -1 || pipe(out_fd) == -1) {
    perror("pipe failed");
    exit(EXIT_FAILURE);
  }

  pid_t pid = fork();

  if (pid == -1) {
    perror("fork failed");
    exit(EXIT_FAILURE);
  } else if (pid == 0) {
    close(in_fd[1]);
    dup2(in_fd[0], STDIN_FILENO);
    close(in_fd[0]);

    close(out_fd[0]);
    dup2(out_fd[1], STDOUT_FILENO);
    dup2(out_fd[1], STDERR_FILENO);
    close(out_fd[1]);

    char* argv[] = {NULL};
    execvp(path, argv);
    perror("execvp failed");
    exit(-1);
  }

  bot_iface->pid = pid;

  close(in_fd[0]);
  close(out_fd[1]);

  bot_iface->writer = fdopen(in_fd[1], "w");
  bot_iface->reader = fdopen(out_fd[0], "r");

  if (!bot_iface->writer || !bot_iface->reader) {
    perror("fdopen failed");
    exit(EXIT_FAILURE);
  }

  bot_iface->path = strdup(path);

  bot_iface->bot_state = BOT_STARTING;

  pthread_create(&bot_iface->msg_reader_handle, NULL, bot_msg_reader,
                 bot_iface);
  send_msg(bot_iface, "uci", 3);

  return 0;
}

int deinit_bot(bot_iface_t* bot_iface) {
  (void)bot_iface->pid;
  return 0;
}

int bot_go(bot_iface_t* bot_iface, const chess_state_t* position,
           const search_config_t* config) {
  bot_iface->bot_state = BOT_RUNNING;
  copy_position(&bot_iface->game_state, position);
  uci_send_position(bot_iface, &bot_iface->game_state);
  uci_send_go(bot_iface, config);
  return 0;
}

int bot_try_get_response(bot_iface_t* bot_iface, move_t* response) {
  switch ((bot_state_t)bot_iface->bot_state) {
    case BOT_RUNNING:
      return 0;
    case BOT_FINISHED:
      *response = bot_iface->bestmove;
      bot_iface->bot_state = BOT_IDLE;
      return 1;
    case BOT_ERROR:
      printf("Illegal move!!!\n");
      return -1;
    case BOT_DEAD: {
      char* path = bot_iface->path;
      bot_iface->path = NULL;
      deinit_bot(bot_iface);
      init_bot(bot_iface, path);
      free(path);
      bot_iface->bot_state = BOT_ERROR;
      return -1;
    }
    case BOT_CLOSED:
    case BOT_PONDERING:
    case BOT_STARTING:
    case BOT_IDLE:
      printf("Ummm... Something went wrong?! Bot should've finished by now!\n");
      bot_iface->bot_state = BOT_ERROR;
      return -1;
    default:
      printf("Invalid bot state: %d\n", bot_iface->bot_state);
      abort();
  }
}

typedef enum outcome_t {
  OUTCOME_INTERNAL_ERROR,
  OUTCOME_BOT1_ERROR,
  OUTCOME_BOT2_ERROR,
  OUTCOME_BOT1_WHITE_CHECKMATE,
  OUTCOME_BOT1_BLACK_CHECKMATE,
  OUTCOME_BOT2_WHITE_CHECKMATE,
  OUTCOME_BOT2_BLACK_CHECKMATE,
  OUTCOME_STALEMATE,
  OUTCOME_DRAW_BY_REPETITION,
  OUTCOME_DRAW_BY_MATERIAL,
  OUTCOME_DRAW_BY_50_MOVES,
  OUTCOME_COUNT,
} outcome_t;

int botvbot(const char* bot1, const char* bot2, int bot_copies,
            int games_to_play) {
  bot_iface_t* bots = calloc(2 * bot_copies, sizeof(*bots));
  chess_state_t* matches = calloc(bot_copies, sizeof(*matches));
  struct {
    int count;
    bot_iface_t* bot_to_move;
    enum {
      STARTING,
      PLAYING,
    } state;
  }* match_states = calloc(bot_copies, sizeof(*match_states));
  size_t outcomes[OUTCOME_COUNT] = {0};
  for (int j = 0; j < bot_copies; j++) {
    if (init_bot(&bots[2 * j], bot1)) {
      printf("failed to load bot!\n");
      return -1;
    }
    if (init_bot(&bots[2 * j + 1], bot2)) {
      printf("failed to load bot!\n");
      return -1;
    }
  }

  games_to_play += bot_copies;
  int active;
  do {
    active = 0;
    for (int i = 0; i < bot_copies; i++) {
      if (match_states[i].state == STARTING && games_to_play <= 0) {
        continue;
      }
      active = 1;
      if (match_states[i].state == STARTING) {
        games_to_play--;
        match_states[i].count++;
        uci_send_ucinewgame(&bots[2 * i]);
        uci_send_ucinewgame(&bots[2 * i + 1]);
        match_states[i].bot_to_move =
            match_states[i].count % 2 == 0 ? &bots[2 * i] : &bots[2 * i + 1];
        load_start_position(&matches[i]);
        bot_go(match_states[i].bot_to_move, &matches[i], NULL);
        match_states[i].state = PLAYING;
      }
      if (match_states[i].state == PLAYING) {
        move_t response;
        int r = bot_try_get_response(match_states[i].bot_to_move, &response);
        if (r == -1) {
          if (match_states[i].bot_to_move == &bots[2 * i]) {
            outcomes[OUTCOME_BOT1_ERROR]++;
          } else {
            outcomes[OUTCOME_BOT2_ERROR]++;
          }
          match_states[i].state = STARTING;
        }
        if (r == 1) {
          make_move(&matches[i], response);
          enum gameover_state gameover = is_gameover(&matches[i]);
          switch (gameover) {
            case ONGOING:
              match_states[i].bot_to_move =
                  match_states[i].bot_to_move == &bots[2 * i] ? &bots[2 * i + 1]
                                                              : &bots[2 * i];
              bot_go(match_states[i].bot_to_move, &matches[i], NULL);
              break;
            case STALEMATE:
              outcomes[OUTCOME_STALEMATE]++;
              match_states[i].state = STARTING;
              break;
            case CHECKMATE:
              if (matches[i].black_to_move) {
                if (match_states[i].bot_to_move == &bots[2 * i]) {
                  outcomes[OUTCOME_BOT1_WHITE_CHECKMATE]++;
                } else {
                  outcomes[OUTCOME_BOT2_WHITE_CHECKMATE]++;
                }
              } else {
                if (match_states[i].bot_to_move == &bots[2 * i]) {
                  outcomes[OUTCOME_BOT1_BLACK_CHECKMATE]++;
                } else {
                  outcomes[OUTCOME_BOT2_BLACK_CHECKMATE]++;
                }
              }
              match_states[i].state = STARTING;
              break;
            case DRAW_BY_50_MOVE_RULE:
              outcomes[OUTCOME_DRAW_BY_50_MOVES]++;
              match_states[i].state = STARTING;
              break;
            case DRAW_BY_INSUFFICIENT_MATERIAL:
              outcomes[OUTCOME_DRAW_BY_MATERIAL]++;
              match_states[i].state = STARTING;
              break;
            case DRAW_BY_REPETITION:
              outcomes[OUTCOME_DRAW_BY_REPETITION]++;
              match_states[i].state = STARTING;
              break;
          }
        }
      }
    }
    usleep(110);
  } while (active);

  // deinit

  printf("BOT1 ERRORS: %zu\n", outcomes[OUTCOME_BOT1_ERROR]);
  printf("BOT2 ERRORS: %zu\n", outcomes[OUTCOME_BOT2_ERROR]);
  printf("BOT1 WHITE WINS: %zu\n", outcomes[OUTCOME_BOT1_WHITE_CHECKMATE]);
  printf("BOT1 BLACK WINS: %zu\n", outcomes[OUTCOME_BOT1_BLACK_CHECKMATE]);
  printf("BOT2 WHITE WINS: %zu\n", outcomes[OUTCOME_BOT2_WHITE_CHECKMATE]);
  printf("BOT2 BLACK WINS: %zu\n", outcomes[OUTCOME_BOT2_BLACK_CHECKMATE]);
  printf("STALEMATES: %zu\n", outcomes[OUTCOME_STALEMATE]);
  printf("DRAW BY REPETITIONS: %zu\n", outcomes[OUTCOME_DRAW_BY_REPETITION]);
  printf("DRAW BY MATERIALS: %zu\n", outcomes[OUTCOME_DRAW_BY_MATERIAL]);
  printf("DRAW BY 50 MOVES: %zu\n", outcomes[OUTCOME_DRAW_BY_50_MOVES]);

  return 0;
}

int main(int argc, const char** argv) {
  if (argc < 3) {
    printf("usage: %s bot1path bot2path\n", argv[0]);
    exit(0);
  }
  botvbot(argv[1], argv[2], 8, 100);
  // bot_iface_t bot_iface1 = {0};
  // int r = init_bot(&bot_iface1, argv[1]);
  // if (r != 0) {
  //   printf("huh\n");
  //   return 1;
  // }
  // bot_iface_t bot_iface2 = {0};
  // r = init_bot(&bot_iface2, argv[1]);
  // if (r != 0) {
  //   printf("huh\n");
  //   return 1;
  // }
  // outcome_t o = play_match(&bot_iface1, &bot_iface2, NULL);
  // switch (o) {
  //   case OUTCOME_INTERNAL_ERROR:
  //     printf("INTERNAL_ERROR\n");
  //     break;
  //   case OUTCOME_WHITE_ERROR:
  //     printf("WHITE_ERROR\n");
  //     break;
  //   case OUTCOME_BLACK_ERROR:
  //     printf("BLACK_ERROR\n");
  //     break;
  //   case OUTCOME_WHITE_CHECKMATE:
  //     printf("WHITE_CHECKMATE\n");
  //     break;
  //   case OUTCOME_BLACK_CHECKMATE:
  //     printf("BLACK_CHECKMATE\n");
  //     break;
  //   case OUTCOME_STALEMATE:
  //     printf("STALEMATE\n");
  //     break;
  //   case OUTCOME_DRAW_BY_REPETITION:
  //     printf("DRAW_BY_REPETITION\n");
  //     break;
  //   case OUTCOME_DRAW_BY_MATERIAL:
  //     printf("DRAW_BY_MATERIAL\n");
  //     break;
  //   case OUTCOME_DRAW_BY_50_MOVES:
  //     printf("DRAW_BY_50_MOVES\n");
  //     break;
  // }
  // return 0;
}