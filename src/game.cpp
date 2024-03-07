#ifdef HAVE_SPDLOG
#include <spdlog/spdlog.h>
#endif
#include <constants.hpp>
#include <game.hpp>

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
#define NULL_MOVE \
  Move { -1, -1 }

#define SCAN_REVERSE_START(x, v) \
  while (x) {                    \
    int pos_##v = bitlen_u128(x) - 1;

#define SCAN_REVERSE_END(x, v)    \
  x ^= ((uint128_t)1 << pos_##v); \
  }

inline bool operator<(const Move &a, const Move &b) {
  return PIECE_DISTANCES[a.dst] - PIECE_DISTANCES[a.src] < PIECE_DISTANCES[b.dst] - PIECE_DISTANCES[b.src];
}

cache::lru_cache<uint64_t, TranspositionTableEntry> HASH_TABLE(1 << 22);

GameState::GameState() : board{0, INITIAL_RED, INITIAL_GREEN}, turn(RED), round(1), zobristHash(0) { hash(); }

GameState::GameState(GameState const &gameState)
    : board{0, gameState.board[RED], gameState.board[GREEN]},
      turn(gameState.turn),
      round(gameState.round),
      zobristHash(gameState.zobristHash) {}

GameState::GameState(const std::string &state) : board{0, 0, 0}, turn(RED), round(10), zobristHash(0) {
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
  hash();
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

std::vector<Move> GameState::legalMoves() {
  std::multiset<Move> moves;
  uint128_t from = board[turn];
  SCAN_REVERSE_START(from, src)
  uint128_t to = ADJ_POSITIONS[pos_src] & ~(board[RED] | board[GREEN]);
  jumpMoves(pos_src, to);
  SCAN_REVERSE_START(to, dst)
  moves.insert({pos_src, pos_dst});
  SCAN_REVERSE_END(to, dst)
  SCAN_REVERSE_END(from, src)
  if (turn == GREEN) {
    return {moves.rbegin(), moves.rend()};
  } else {
    return {moves.begin(), moves.end()};
  }
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
  if (zobristHash != 0) {
    zobristHash ^= ZOBRIST_TABLE[move.src][turn];
    zobristHash ^= ZOBRIST_TABLE[move.dst][turn];
    zobristHash ^= 0xc503204d9e521ac5ULL;
  }
  board[turn] ^= (uint128_t)1 << move.src;
  board[turn] |= (uint128_t)1 << move.dst;
  turn = turn == Color::RED ? Color::GREEN : Color::RED;
  if (turn == RED) {
    round++;
  }
}

void GameState::undoMove(Move move) {
  turn = turn == Color::RED ? Color::GREEN : Color::RED;
  board[turn] ^= (uint128_t)1 << move.dst;
  board[turn] |= (uint128_t)1 << move.src;
  if (turn == RED) {
    round--;
  }
  if (zobristHash != 0) {
    zobristHash ^= ZOBRIST_TABLE[move.src][turn];
    zobristHash ^= ZOBRIST_TABLE[move.dst][turn];
    zobristHash ^= 0xc503204d9e521ac5ULL;
  }
}

int GameState::evaluate() {
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
  redScore -= 1 << std::max(0, 4 - lastRed);
  greenScore -= 1 << std::max(0, 4 - lastGreen);
  if (lastRed == 13) {
    redScore = 10000;
    greenScore = 0;
  }
  if (lastGreen == 13) {
    greenScore = 10000;
    redScore = 0;
  }
  return turn == Color::RED ? redScore - greenScore : greenScore - redScore;
}

uint64_t GameState::hash() {
  if (zobristHash == 0) {
    uint128_t red = board[RED];
    uint128_t green = board[GREEN];
    SCAN_REVERSE_START(red, src)
    zobristHash ^= ZOBRIST_TABLE[pos_src][RED];
    SCAN_REVERSE_END(red, src)
    SCAN_REVERSE_START(green, src)
    zobristHash ^= ZOBRIST_TABLE[pos_src][GREEN];
    SCAN_REVERSE_END(green, src)
    if (turn == GREEN) {
      zobristHash ^= 0xc503204d9e521ac5ULL;
    }
  }
  return zobristHash;
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

Move GameState::searchBestMove(int timeLimit) {
  if (round <= 4) {
    // 开局库
    return OPENINGS[turn].at(board[turn]);
  }
  int depth = 1, eval = -INF, bestEval = -INF;
  Move move = NULL_MOVE, bestMove = NULL_MOVE;
  auto deadline = SECONDS_LATER(timeLimit);
  MovePath pline;
  while (depth < 100) {
    pline.moves.clear();
    bestEval = eval;
    bestMove = move;
    eval = mtdf(*this, depth, eval, pline, deadline);
    uint64_t h = hash();
    move = HASH_TABLE.get(h).bestMove;
#ifdef HAVE_SPDLOG
    spdlog::info("complete search depth: {}, score: {}, move: {} {}", depth, eval, move.src, move.dst);
#endif
    auto tempState = *this;
    while (HASH_TABLE.exists(h)) {
      auto result = HASH_TABLE.get(h);
      pline.moves.push_back(result.bestMove);
      tempState.applyMove(result.bestMove);
      h = tempState.hash();
    }
    if (eval > 9999 || NOW >= deadline) {
      // 找到胜利着法
      break;
    }
    depth++;
  }
  if (eval > bestEval) {
    bestEval = eval;
    bestMove = move;
  }
#ifdef HAVE_SPDLOG
  spdlog::info("final eval: {}", bestEval);
#endif
  return bestMove;
}

int mtdf(GameState &gameState, int depth, int guess, MovePath &pline, time_point_t deadline) {
  int beta;
  int upperbound = INF;
  int lowerbound = -INF;
  int score = guess;
  do {
    beta = (score == lowerbound ? score + 1 : score);
    pline.index = 0;
    score = alphaBetaSearch(gameState, depth, beta - 1, beta, pline, deadline);
    (score < beta ? upperbound : lowerbound) = score;
  } while (lowerbound < upperbound);
  return score;
}

int alphaBetaSearch(GameState &gameState, int depth, int alpha, int beta, MovePath &pline, time_point_t deadline) {
  // 查询置换表
  uint64_t hash = gameState.hash();
  int alphaOrig = alpha;
  if (HASH_TABLE.exists(hash)) {
    auto result = HASH_TABLE.get(hash);
    if (result.depth >= depth) {
      if (result.flag == HASH_EXACT) {
        return result.value;
      } else if (result.flag == HASH_LOWERBOUND) {
        alpha = std::max(alpha, result.value);
      } else if (result.flag == HASH_UPPERBOUND) {
        beta = std::min(beta, result.value);
      }
      if (alpha >= beta) {
        return result.value;
      }
    }
  }

  // 叶子结点
  if (gameState.isGameOver() || depth == 0) {
    return gameState.evaluate();
  }

  Move bestMove = NULL_MOVE;
  HashFlag flag;
  int value = -INF;
  auto moves = gameState.legalMoves();
  // 优先上一次搜索的最佳着法
  if (pline.index != pline.moves.size()) {
    moves.insert(moves.begin(), pline.moves[pline.index++]);
  }
  for (Move move : moves) {
    // 跳过向后走两步及其以上的着法
    if (gameState.getTurn() == GREEN && PIECE_DISTANCES[move.dst] - PIECE_DISTANCES[move.src] <= -2) {
      continue;
    } else if (gameState.getTurn() == RED && PIECE_DISTANCES[move.src] - PIECE_DISTANCES[move.dst] <= -2) {
      continue;
    }
    gameState.applyMove(move);
    int current = -alphaBetaSearch(gameState, depth - 1, -beta, -alpha, pline, deadline);
    gameState.undoMove(move);
    if (current > value) {
      value = current;
      bestMove = move;
    }
    alpha = std::max(alpha, value);
    if (alpha >= beta) {
      // 发生截断
      break;
    }
    // 超时检测
    if (NOW >= deadline) {
      break;
    }
  }
  if (value <= alphaOrig) {
    flag = HASH_UPPERBOUND;
  } else if (value >= beta) {
    flag = HASH_LOWERBOUND;
  } else {
    flag = HASH_EXACT;
  }
  HASH_TABLE.put(hash, {hash, value, depth, flag, bestMove});
  return alpha;
}
