#include <spdlog/spdlog.h>

#include <cache.hpp>
#include <constants.hpp>
#include <game.hpp>
#include <limits>
#include <string>

inline int bitlen_u128(uint128_t u) {
  if (u == 0) {
    return 0;
  }
  uint64_t upper = u >> 64;
  if (upper) {
    return 128 - __builtin_clzll(upper);
  } else {
    return 64 - __builtin_clzll((uint64_t)u);
  }
}

#define INF INT_MAX
#define SECONDS_LATER(x) std::chrono::high_resolution_clock::now() + std::chrono::seconds(x)
#define NOW std::chrono::high_resolution_clock::now()

#define SCAN_REVERSE_START(x, v) \
  while (x) {                    \
    int pos_##v = bitlen_u128(x) - 1;

#define SCAN_REVERSE_END(x, v)    \
  x ^= ((uint128_t)1 << pos_##v); \
  }

cache::lru_cache<uint64_t, TranspositionTableEntry> HASH_TABLE(1 << 20);

GameState::GameState() : board{0, INITIAL_RED, INITIAL_GREEN}, turn(RED), round(1) {}

GameState::GameState(GameState const &gameState)
    : board{0, gameState.board[RED], gameState.board[GREEN]}, turn(gameState.turn), round(gameState.round) {}

GameState::GameState(const std::string &state) : board{0, 0, 0}, turn(RED), round(10) {
  board[RED] = 0;
  board[GREEN] = 0;
  int p = 0;
  int i = 0;
  for (char c : state) {
    switch (c) {
      case '0':
        p++;
        break;
      case '1':
        board[RED] |= (uint128_t)1 << p++;
        break;
      case '2':
        board[GREEN] |= (uint128_t)1 << p++;
        break;
      case 'r':
        turn = RED;
        goto end;
      case 'g':
        turn = GREEN;
        goto end;
      default:
        break;
    }
    i++;
  }
end:
  try {
    round = std::stoi(state.substr(i + 1));
  } catch (std::invalid_argument const &e) {
    round = 10;
  }
}

std::string GameState::toString() {
  std::string result;
  for (int i = 0; i < 81; i++) {
    if (board[RED] >> i & 1) {
      result += "1";
    } else if (board[GREEN] >> i & 1) {
      result += "2";
    } else {
      result += "0";
    }
    if (i % 9 == 8 && i != 80) {
      result += "/";
    }
  }
  result += turn == RED ? " r " : " g ";
  result += std::to_string(round);
  return result;
}

std::vector<int> GameState::getBoard() {
  std::vector<int> result;
  for (int i = 0; i < 81; i++) {
    if (board[RED] >> i & 1) {
      result.push_back(RED);
    } else if (board[GREEN] >> i & 1) {
      result.push_back(GREEN);
    } else {
      result.push_back(EMPTY);
    }
  }
  return result;
}

Color GameState::getTurn() const { return turn; }

std::vector<Move> GameState::legalMoves(Color color) {
  if (color == EMPTY) {
    color = turn;
  }
  std::vector<Move> moves;
  uint128_t from = board[color];
  SCAN_REVERSE_START(from, src)
  uint128_t to = ADJ_POSITIONS[pos_src] & ~(board[RED] | board[GREEN]);
  jumpMoves(pos_src, to);
  SCAN_REVERSE_START(to, dst)
  moves.push_back({pos_src, pos_dst});
  SCAN_REVERSE_END(to, dst)
  SCAN_REVERSE_END(from, src)
  // sort moves
  std::sort(moves.begin(), moves.end(), [color](Move a, Move b) {
    return color == RED
               ? PIECE_DISTANCES[a.dst] - PIECE_DISTANCES[a.src] < PIECE_DISTANCES[b.dst] - PIECE_DISTANCES[b.src]
               : PIECE_DISTANCES[a.dst] - PIECE_DISTANCES[a.src] > PIECE_DISTANCES[b.dst] - PIECE_DISTANCES[b.src];
  });
  return moves;
}

void GameState::jumpMoves(int src, uint128_t &to) {
  uint128_t jumps = JUMP_POSITIONS[src].at(ADJ_POSITIONS[src] & (board[RED] | board[GREEN]));
  jumps &= ~(board[RED] | board[GREEN]);
  if ((jumps | to) == to) {
    return;
  }
  to |= jumps;
  SCAN_REVERSE_START(jumps, dst)
  jumpMoves(pos_dst, to);
  SCAN_REVERSE_END(jumps, dst)
}

void GameState::applyMove(Move move) {
  board[turn] ^= (uint128_t)1 << move.src;
  board[turn] |= (uint128_t)1 << move.dst;
  turn = turn == Color::RED ? Color::GREEN : Color::RED;
  if (turn == RED) {
    round++;
  }
}

int GameState::evaluate(int maxiumColor) {
  int redScore = 0;
  int greenScore = 0;
  int lastRed = INF;
  int lastGreen = INF;
  uint128_t red = board[RED];
  uint128_t green = board[GREEN];
  SCAN_REVERSE_START(red, src)
  if (PIECE_DISTANCES[80 - pos_src] < lastRed) {
    lastRed = PIECE_DISTANCES[80 - pos_src];
  }
  redScore += PIECE_SCORE_TABLE[80 - pos_src];
  SCAN_REVERSE_END(red, src)
  SCAN_REVERSE_START(green, src)
  if (PIECE_DISTANCES[pos_src] < lastGreen) {
    lastGreen = PIECE_DISTANCES[pos_src];
  }
  greenScore += PIECE_SCORE_TABLE[pos_src];
  SCAN_REVERSE_END(green, src)
  redScore -= 1 << std::max(0, 5 - lastRed);
  greenScore -= 1 << std::max(0, 5 - lastGreen);
  if (lastRed == 13) {
    redScore = 10000;
    greenScore = 0;
  }
  if (lastGreen == 13) {
    greenScore = 10000;
    redScore = 0;
  }
  return maxiumColor == Color::RED ? redScore - greenScore : greenScore - redScore;
}

uint64_t GameState::hash() {
  uint64_t hash = 0;
  for (int i = 0; i < 81; i++) {
    if (board[RED] >> i & 1) {
      hash ^= ZOBRIST_TABLE[i][RED];
    } else if (board[GREEN] >> i & 1) {
      hash ^= ZOBRIST_TABLE[i][GREEN];
    }
  }
  if (turn == GREEN) {
    hash ^= 0xc503204d9e521ac5ULL;
  }
  return hash;
}

bool GameState::isGameOver() {
  bool redWin = true;
  bool greenWin = true;
  uint128_t red = board[RED];
  uint128_t green = board[GREEN];
  SCAN_REVERSE_START(red, src)
  if (PIECE_DISTANCES[pos_src] > 3) {
    redWin = false;
  }
  SCAN_REVERSE_END(red, src)
  SCAN_REVERSE_START(green, src)
  if (PIECE_DISTANCES[pos_src] < 13) {
    greenWin = false;
  }
  SCAN_REVERSE_END(green, src)
  return redWin || greenWin;
}

Move GameState::searchBestMove(int depth) {
  if (round <= 4) {
    return OPENINGS[turn].at(board[turn]);
  }
  int score = 0, beta, lowerbound = -INF, upperbound = INF;
  Move move = {-1, -1};
  try {
    // MTD(f) search
    do {
      beta = (score == lowerbound ? score + 1 : score);
      std::tie(move, score) = maxValue(*this, depth, beta - 1, beta, turn, SECONDS_LATER(60));
      spdlog::info("search depth: {}, score: {}, beta: {}", depth, score, beta);
      (score < beta ? upperbound : lowerbound) = score;
    } while (lowerbound < upperbound);
  } catch (std::runtime_error &e) {
    spdlog::error(e.what());
  }
  spdlog::info("evaluated score: {}", score);
  return move;
}

Move GameState::searchBestMoveWithTimeLimit(int timeLimit) {
  if (round <= 4) {
    return OPENINGS[turn].at(board[turn]);
  }
  int bestEval = -INF, depth = 1, eval;
  Move bestMove = {-1, -1}, move = {-1, -1};
  auto deadline = SECONDS_LATER(timeLimit);
  while (depth < 100) {
    try {
      std::tie(move, eval) = maxValue(*this, depth, -INF, INF, turn, deadline);
    } catch (std::runtime_error &e) {
      spdlog::info("actual search depth: {}", depth - 1);
      break;
    }
    if (eval > bestEval) {
      bestEval = eval;
      bestMove = move;
    }
    if (eval > 9999) {
      break;
    }
    depth++;
  }
  spdlog::info("evaluated score: {}", eval);
  return bestMove;
}

std::pair<Move, int> maxValue(GameState &gameState, int depth, int alpha, int beta, int maxiumColor,
                              std::chrono::time_point<std::chrono::high_resolution_clock> deadline) {
  // 查询置换表
  uint64_t hash = gameState.hash();
  if (HASH_TABLE.exists(hash)) {
    auto result = HASH_TABLE.get(hash);
    if (result.depth >= depth && result.maxiumColor == maxiumColor) {
      if (result.flag == HASH_EXACT) {
        return {result.bestMove, result.value};
      } else if (result.flag == HASH_BETA && result.value >= beta) {
        return {result.bestMove, beta};
      }
    }
  }

  if (gameState.isGameOver() || depth == 0) {
    return {{Move{-2, -2}}, gameState.evaluate(maxiumColor)};
  }
  Move bestMove = {-3, -3};
  HashFlag flag = HASH_ALPHA;
  for (Move move : gameState.legalMoves()) {
    GameState newState(gameState);
    newState.applyMove(move);
    int eval = minValue(newState, depth - 1, alpha, beta, maxiumColor, deadline).second;
    if (eval > alpha) {
      alpha = eval;
      bestMove = move;
      flag = HASH_EXACT;
      if (beta <= alpha) {
        // 发生 Beta 截断 Fail-High
        HASH_TABLE.put(hash, {alpha, depth, HASH_BETA, bestMove, maxiumColor});
        return {bestMove, alpha};
      }
    }
    // 超时检测
    if (NOW >= deadline) {
      throw std::runtime_error("timeout");
    }
  }
  HASH_TABLE.put(hash, {alpha, depth, flag, bestMove, maxiumColor});
  return {bestMove, alpha};
}

std::pair<Move, int> minValue(GameState &gameState, int depth, int alpha, int beta, int maxiumColor,
                              std::chrono::time_point<std::chrono::high_resolution_clock> deadline) {
  // 查询置换表
  uint64_t hash = gameState.hash();
  if (HASH_TABLE.exists(hash)) {
    auto result = HASH_TABLE.get(hash);
    if (result.depth >= depth && result.maxiumColor == maxiumColor) {
      if (result.flag == HASH_EXACT) {
        return {result.bestMove, result.value};
      } else if (result.flag == HASH_ALPHA && result.value <= alpha) {
        return {result.bestMove, alpha};
      }
    }
  }

  if (gameState.isGameOver() || depth == 0) {
    return {{Move{-2, -2}}, gameState.evaluate(maxiumColor)};
  }
  Move bestMove = {-3, -3};
  HashFlag flag = HASH_BETA;
  for (Move move : gameState.legalMoves()) {
    GameState newState(gameState);
    newState.applyMove(move);
    int eval = maxValue(newState, depth - 1, alpha, beta, maxiumColor, deadline).second;
    if (eval < beta) {
      beta = eval;
      bestMove = move;
      flag = HASH_EXACT;
      if (beta <= alpha) {
        // 发生 Alpha 截断 Fail-Low
        HASH_TABLE.put(hash, {beta, depth, HASH_ALPHA, bestMove, maxiumColor});
        return {bestMove, beta};
      }
    }
    // 超时检测
    if (NOW >= deadline) {
      throw std::runtime_error("timeout");
    }
  }
  HASH_TABLE.put(hash, {beta, depth, flag, bestMove, maxiumColor});
  return {bestMove, beta};
}
