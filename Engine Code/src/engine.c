#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Minimal UCI engine: lc0-powered with alpha-beta fallback.
// No en-passant (filtered out); promotions -> queen only.
// Threefold-repetition detection prevents repeat moves.

#define LC0_EXE_NAME      "lc0\\lc0.exe"
#define LC0_WEIGHTS_FILE  "lc0\\network.pb"
#define LC0_MOVETIME_MS   2000
#define FALLBACK_DEPTH    5

/* ── Path resolution ─────────────────────────────────────────────────────── */
static char g_exe_dir[MAX_PATH] = {0};

static void get_exe_dir(void) {
    char full[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, full, MAX_PATH);
    char *sep = strrchr(full, '\\');
    if (sep) { *sep = '\0'; strncpy(g_exe_dir, full, MAX_PATH - 1); }
    else      { strncpy(g_exe_dir, full, MAX_PATH - 1); }
}

static void build_sibling_path(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "%s\\%s", g_exe_dir, name);
}

typedef struct {
    int from, to;
    char promo;
} Move;

typedef struct {
    char b[64];
    int white_to_move;
    int cK, cQ, ck, cq;  // castling rights, capital = white, else = black
} Pos;

static int sq_index(const char *s) {
    int file = s[0] - 'a';
    int rank = s[1] - '1';
    return rank * 8 + file;
}

static void index_to_sq(int idx, char out[3]) {
    out[0] = (char) ('a' + (idx % 8));
    out[1] = (char) ('1' + (idx / 8));
    out[2] = 0;
}

static void pos_from_fen(Pos *p, const char *fen) {
    memset(p->b, '.', 64);
    p->white_to_move = 1;
    p->cK = 0;
    p->cQ = 0;
    p->ck = 0;
    p->cq = 0;

    char buf[256];
    strncpy(buf, fen, sizeof(buf)-1);
    buf[sizeof(buf) - 1] = 0;

    char *save = NULL;
    char *placement = strtok_r(buf, " ", &save);
    char *stm = strtok_r(NULL, " ", &save);
    char *castling = strtok_r(NULL, " ", &save);
    if (stm) p->white_to_move = (strcmp(stm, "w") == 0);

    if (castling && strcmp(castling, "-") != 0) {
        for (int i = 0; castling[i]; i++) {
            switch (castling[i]) {
                case 'K': p->cK = 1; break;
                case 'Q': p->cQ = 1; break;
                case 'k': p->ck = 1; break;
                case 'q': p->cq = 1; break;
            }
        }
    }

    int rank = 7, file = 0;
    for (size_t i = 0; placement && placement[i]; i++) {
        char c = placement[i];
        if (c == '/') {
            rank--;
            file = 0;
            continue;
        }
        if (isdigit((unsigned char) c)) {
            file += c - '0';
            continue;
        }
        int idx = rank * 8 + file;
        if (idx >= 0 && idx < 64) p->b[idx] = c;
        file++;
    }
}

static void pos_start(Pos *p) {
    pos_from_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

static int is_white_piece(char c) { return c >= 'A' && c <= 'Z'; }

static int is_square_attacked(const Pos *p, int sq, int by_white) {
    int r = sq / 8, f = sq % 8;

    // pawns
    if (by_white) {
        if (r > 0 && f > 0 && p->b[(r - 1) * 8 + (f - 1)] == 'P') return 1;
        if (r > 0 && f < 7 && p->b[(r - 1) * 8 + (f + 1)] == 'P') return 1;
    } else {
        if (r < 7 && f > 0 && p->b[(r + 1) * 8 + (f - 1)] == 'p') return 1;
        if (r < 7 && f < 7 && p->b[(r + 1) * 8 + (f + 1)] == 'p') return 1;
    }

    // knights
    static const int nd[8] = {-17, -15, -10, -6, 6, 10, 15, 17};
    for (int i = 0; i < 8; i++) {
        int to = sq + nd[i];
        if (to < 0 || to >= 64) continue;
        int tr = to / 8, tf = to % 8;
        int dr = tr - r;
        if (dr < 0) dr = -dr;
        int df = tf - f;
        if (df < 0) df = -df;
        if (!((dr == 1 && df == 2) || (dr == 2 && df == 1))) continue;
        char pc = p->b[to];
        if (by_white && pc == 'N') return 1;
        if (!by_white && pc == 'n') return 1;
    }

    // sliders
    static const int dirs[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };

    for (int di = 0; di < 8; di++) {
        int df = dirs[di][0], dr = dirs[di][1];
        int cr = r + dr, cf = f + df;
        while (cr >= 0 && cr < 8 && cf >= 0 && cf < 8) {
            int idx = cr * 8 + cf;
            char pc = p->b[idx];
            if (pc != '.') {
                int pc_white = is_white_piece(pc);
                if (pc_white == by_white) {
                    char up = (char) toupper((unsigned char) pc);
                    int rook_dir = (di < 4);
                    int bishop_dir = (di >= 4);
                    if (up == 'Q') return 1;
                    if (rook_dir && up == 'R') return 1;
                    if (bishop_dir && up == 'B') return 1;
                    if (up == 'K' && (abs(cr - r) <= 1 && abs(cf - f) <= 1)) return 1;
                }
                break;
            }
            cr += dr;
            cf += df;
        }
    }

    // king adjacency (extra safety)
    for (int rr = r - 1; rr <= r + 1; rr++) {
        for (int ff = f - 1; ff <= f + 1; ff++) {
            if (rr < 0 || rr >= 8 || ff < 0 || ff >= 8) continue;
            if (rr == r && ff == f) continue;
            char pc = p->b[rr * 8 + ff];
            if (by_white && pc == 'K') return 1;
            if (!by_white && pc == 'k') return 1;
        }
    }

    return 0;
}

static int in_check(const Pos *p, int white_king) {
    char k = white_king ? 'K' : 'k';
    int ksq = -1;
    for (int i = 0; i < 64; i++) if (p->b[i] == k) {
        ksq = i;
        break;
    }
    if (ksq < 0) return 1;
    return is_square_attacked(p, ksq, !white_king);
}

static Pos make_move(const Pos *p, Move m) {
    Pos np = *p;
    char piece = np.b[m.from];
    np.b[m.from] = '.';
    char placed = piece;
    if (m.promo && (piece == 'P' || piece == 'p')) {
        placed = is_white_piece(piece)
                     ? (char) toupper((unsigned char) m.promo)
                     : (char) tolower((unsigned char) m.promo);
    }
    np.b[m.to] = placed;
    np.white_to_move = !p->white_to_move;

    if (piece == 'K') { np.cK = 0; np.cQ = 0; }
    if (piece == 'k') { np.ck = 0; np.cq = 0; }
 
    if (m.from == 0  || m.to == 0)  np.cQ = 0; 
    if (m.from == 7  || m.to == 7)  np.cK = 0; 
    if (m.from == 56 || m.to == 56) np.cq = 0;  
    if (m.from == 63 || m.to == 63) np.ck = 0;  

    if (piece == 'K') {
        if (m.from == 4 && m.to == 6) { 
            np.b[5] = 'R'; np.b[7] = '.';  
        } else if (m.from == 4 && m.to == 2) { 
            np.b[3] = 'R'; np.b[0] = '.';    
        }
    }
    if (piece == 'k') {
        if (m.from == 60 && m.to == 62) {    
            np.b[61] = 'r'; np.b[63] = '.';   
        } else if (m.from == 60 && m.to == 58) { 
            np.b[59] = 'r'; np.b[56] = '.';    
        }
    }
    return np;
}

static void add_move(Move *moves, int *n, int from, int to, char promo) {
    moves[*n].from = from;
    moves[*n].to = to;
    moves[*n].promo = promo;
    (*n)++;
}

char promote_pawn(const Pos *p, int to, int white) {
    int r = (to / 8) + 1;
    char promo_stat;
    promo_stat = '\0'; //make a char that is 0 for boolean logic, not actual 0 on ASCII table
    if (white == 1) {
        if (r == 8) {
            promo_stat = 'Q';
        }
    }

    else if (white == 0) {
        if (r == 1) {
            promo_stat = 'q';
        }
    }

    return promo_stat;
}

static void eval_castling(const Pos *p, int white, Move *moves, int *n) {
    if (in_check(p, white)) return;
    if (white) {
        if (p->cK && p->b[4] == 'K' && p->b[7] == 'R' && p->b[5] == '.'      
            && p->b[6] == '.' && !is_square_attacked(p, 5, !white)) {
            add_move(moves, n, 4, 6, 0);
        }
        if (p->cQ && p->b[4] == 'K' && p->b[0] == 'R' && p->b[1] == '.'        
            && p->b[2] == '.' && p->b[3] == '.' && !is_square_attacked(p, 3, !white)) {
            add_move(moves, n, 4, 2, 0);
        }
    } else {
        if (p->ck && p->b[60] == 'k' && p->b[63] == 'r' && p->b[61] == '.'       
            && p->b[62] == '.' && !is_square_attacked(p, 61, !white)) {
            add_move(moves, n, 60, 62, 0);
        }
        if (p->cq && p->b[60] == 'k' && p->b[56] == 'r' && p->b[57] == '.'  
            && p->b[58] == '.' && p->b[59] == '.' && !is_square_attacked(p, 59, !white)) {
            add_move(moves, n, 60, 58, 0);
        }
    }
}

static void gen_pawn(const Pos *p, int from, int white, Move *moves, int *n) {
    int r = (from / 8) + 1;
    int f = from % 8;

    static const int nd[8] = { 8, 16, 7, 9, -8, -16, -7, -9 };
    for (int i = 0; i < 8; i++) {
        int to = from + nd[i];
        if (to < 0 || to >= 64) continue;
        int tf = to % 8;
        char toch = p->b[to];
        char promo = promote_pawn(p, to, white);

        if (white == 1) {
            if ((to == (from + 8)) && (toch == '.')) {
                add_move(moves, n, from, to, promo);
            }
            if ((r == 2) && (to == (from + 16)) && (toch == '.') && p->b[from + 8] == '.') {
                add_move(moves, n, from, to, promo);
            }
            if ((to == (from + 7)) && toch != '.' && !(is_white_piece(toch)) && (tf == f - 1)) {
                add_move(moves, n, from, to, promo);
            }
            if ((to == (from + 9)) && toch != '.' && !(is_white_piece(toch)) && (tf == f + 1)) {
                add_move(moves, n, from, to, promo);
            }
        }
        else if (white == 0) {
            if ((to == (from - 8)) && (toch == '.')) {
                add_move(moves, n, from, to, promo);
            }
            if ((r == 7) && (to == (from - 16)) && (toch == '.') && p->b[from - 8] == '.') {
                add_move(moves, n, from, to, promo);
            }
            if ((to == (from - 7)) && toch != '.' && is_white_piece(toch) && (tf == f + 1)) {
                add_move(moves, n, from, to, promo);
            }
            if ((to == (from - 9)) && toch != '.' && is_white_piece(toch) && (tf == f - 1)) {
                add_move(moves, n, from, to, promo);
            }
        }
    }
}

static void gen_knight(const Pos* p, int from, int white, Move* moves, int* n) {
    int r = from / 8, f = from % 8; //get rank/file of the knight
    static const int nd[8] = {-17, -15, -10, -6, 6, 10, 15, 17}; //knight move offsets
    for (int i = 0; i < 8; i++) {  //for each possible knight move
        int to = from + nd[i]; //calculate target square index
        if (to < 0 || to >= 64) continue; //skip if target square is off the board
        int tr = to / 8, tf = to % 8; //get rank/file of the target square
        int dr = tr - r; //get rank difference
        if (dr < 0) dr = -dr; //absolute value of rank difference
        int df = tf - f; //get file difference
        if (df < 0) df = -df; //absolute value of file difference
        if (!((dr == 1 && df == 2) || (dr == 2 && df == 1))) continue;  //skip if not a valid knight move
        char pc = p->b[to]; //get piece on target square
        if (pc == '.' || is_white_piece(pc) != white) { //if target square is empty or occupied by opponent's piece
            add_move(moves, n, from, to, 0); //add move to the list of moves
        }
    }
}

static void gen_queen(const Pos *p, int from, int white, const int dirs[][2], int dcount, Move *moves, int *n) {
    int r = from / 8;
    int f = from % 8;
    
    // must check all possible directions, dcount = 8
    for (int di = 0; di < dcount; di++) {
        int df = dirs[di][0];
        int dr = dirs[di][1];
        int cr = r + dr;
        int cf = f + df;

        while (cr >= 0 && cr < 8 && cf >= 0 && cf < 8) {
            int to = cr * 8 + cf;
            char target = p->b[to];

            if (target == '.') {
                add_move(moves, n, from, to, 0);
            } else {
                int target_white = is_white_piece(target);
                if (target_white != white) {
                    add_move(moves, n, from, to, 0);
                }
                break;
            }
            cr += dr;
            cf += df;
        }
    }
}

static void gen_bishop(const Pos *p, int from, int white, const int dirs[][2], int dcount, Move *moves, int *n) {
    int r = from / 8;
    int f = from % 8;

    // diagonal slider only, dcount = 4
    for (int di = 0; di < dcount; di++) {
        int df = dirs[di][0];
        int dr = dirs[di][1];
        int cr = r + dr;
        int cf = f + df;

        while (cr >= 0 && cr < 8 && cf >= 0 && cf < 8) {
            int to = cr * 8 + cf;
            char target = p->b[to];

            if (target == '.') {
                add_move(moves, n, from, to, 0);
            } else {
                int target_white = is_white_piece(target);
                if (target_white != white) {
                    add_move(moves, n, from, to, 0);
                }
                break;
            }
            cr += dr;
            cf += df;
        }
    }
}

static void gen_rook(const Pos *p, int from, int white, const int dirs[][2], int dcount, Move *moves, int *n) {
    int r = from / 8;
    int f = from % 8;

    // horizontal and vertical slider only, dcount = 4
    for (int di = 0; di < dcount; di++) {
        int df = dirs[di][0];
        int dr = dirs[di][1];
        int cr = r + dr;
        int cf = f + df;

        while (cr >= 0 && cr < 8 && cf >= 0 && cf < 8) {
            int to = cr * 8 + cf;
            char target = p->b[to];

            if (target == '.') {
                add_move(moves, n, from, to, 0);
            } else {
                int target_white = is_white_piece(target);
                if (target_white != white) {
                    add_move(moves, n, from, to, 0);
                }
                break;
            }
            cr += dr;
            cf += df;
        }
    }
}

static void gen_king(const Pos *p, int from, int white, const int dirs[][2], int dcount, Move *moves, int *n) {
    int r = from / 8;
    int f = from % 8;

    // horizontal and vertical directionality, but only for one square, dcount = 8
    for (int di = 0; di < dcount; di++) {
        int df = dirs[di][0];
        int dr = dirs[di][1];
        int cr = r + dr;
        int cf = f + df;

        if (cr >= 0 && cr < 8 && cf >= 0 && cf < 8) {
            int to = cr * 8 + cf;
            char target = p->b[to];

            if (target == '.') {
                add_move(moves, n, from, to, 0);
            } else {
                int target_white = is_white_piece(target);
                if (target_white != white) {
                    add_move(moves, n, from, to, 0);
                }
            }
        }
    }
    eval_castling(p, white, moves, n);
}

static int pseudo_legal_moves(const Pos *p, Move *moves) {
    int n = 0;
    int us_white = p->white_to_move;
    for (int i = 0; i < 64; i++) {
        char pc = p->b[i];
        if (pc == '.') continue;
        int white = is_white_piece(pc);
        if (white != us_white) continue;
        char up = (char) toupper((unsigned char) pc);
        if (up == 'P') gen_pawn(p, i, white, moves, &n);
        else if (up == 'N') gen_knight(p, i, white, moves, &n);
        else if (up == 'B') {
            static const int d[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            gen_bishop(p, i, white, d, 4, moves, &n);
        } else if (up == 'R') {
            static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            gen_rook(p, i, white, d, 4, moves, &n);
        } else if (up == 'Q') {
            static const int d[8][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            gen_queen(p, i, white, d, 8, moves, &n);
        } else if (up == 'K') {
            static const int d[8][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            gen_king(p, i, white, d, 8, moves, &n);
        }
    }
    return n;
}

static int legal_moves(const Pos *p, Move *out) {
    Move tmp[256];
    int pn = pseudo_legal_moves(p, tmp);
    int n = 0;
    for (int i = 0; i < pn; i++) {
        Pos np = make_move(p, tmp[i]);
        // after move, side who just moved is !np.white_to_move
        if (!in_check(&np, !np.white_to_move)) {
            out[n++] = tmp[i];
        }
    }
    return n;
}

/* ── En-passant filter ───────────────────────────────────────────────────── */
static int is_en_passant(const Pos *p, Move m) {
    char pc = p->b[m.from];
    if (toupper((unsigned char) pc) != 'P') return 0;
    return (m.from % 8 != m.to % 8) && (p->b[m.to] == '.');
}

/* ── Threefold-repetition filter  ────────────────── */
#define MAX_HISTORY 512

typedef struct {
    char b[64];
    int white_to_move;
} BoardSnap;

static BoardSnap g_pos_history[MAX_HISTORY];
static int       g_history_len = 0;

/* Move string history used to rebuild the position command for lc0 */
static char g_move_history[MAX_HISTORY][6];
static int  g_move_history_len = 0;
static int  g_pos_is_startpos  = 1;
static char g_pos_fen[512]     = {0};
static int  g_halfmove_clock   = 0; // moves since last pawn move or capture

static int snaps_equal(const BoardSnap *a, const BoardSnap *b) {
    return a->white_to_move == b->white_to_move
        && memcmp(a->b, b->b, 64) == 0;
}

static int snap_count(const BoardSnap *snap) {
    int c = 0;
    for (int i = 0; i < g_history_len; i++)
        if (snaps_equal(&g_pos_history[i], snap)) c++;
    return c;
}

/* Returns 1 if playing m would create the 3rd occurrence of a position */
static int is_repetition_move(const Pos *p, Move m) {
    Pos np = make_move(p, m);
    BoardSnap snap;
    memcpy(snap.b, np.b, 64);
    snap.white_to_move = np.white_to_move;
    return snap_count(&snap) >= 2;
}

/* Returns 1 if the current position is already drawn by threefold repetition */
static int is_threefold_draw(void) {
    if (g_history_len < 1) return 0;
    BoardSnap *cur = &g_pos_history[g_history_len - 1];
    return snap_count(cur) >= 3;
}

/* Returns 1 if there is insufficient material for either side to checkmate */
static int is_insufficient_material(const Pos *p) {
    int wp = 0, wn = 0, wb = 0, wr = 0, wq = 0;
    int bp = 0, bn = 0, bb = 0, br = 0, bq = 0;
    int wb_light = 0, wb_dark = 0, bb_light = 0, bb_dark = 0;
    for (int i = 0; i < 64; i++) {
        char pc = p->b[i];
        if (pc == '.') continue;
        int light = ((i / 8) + (i % 8)) % 2 == 0;
        switch (pc) {
            case 'P': wp++; break; case 'N': wn++; break;
            case 'B': wb++; if (light) wb_light++; else wb_dark++; break;
            case 'R': wr++; break; case 'Q': wq++; break;
            case 'p': bp++; break; case 'n': bn++; break;
            case 'b': bb++; if (light) bb_light++; else bb_dark++; break;
            case 'r': br++; break; case 'q': bq++; break;
        }
    }
    // any pawns, rooks or queens -> not insufficient
    if (wp || bp || wr || br || wq || bq) return 0;
    // K vs K
    if (!wn && !wb && !bn && !bb) return 1;
    // K+B vs K or K+N vs K
    if (!bn && !bb && (wn + wb) <= 1) return 1;
    if (!wn && !wb && (bn + bb) <= 1) return 1;
    // K+B vs K+B same color bishops
    if (!wn && !bn && wb == 1 && bb == 1) {
        if ((wb_light && bb_light) || (wb_dark && bb_dark)) return 1;
    }
    return 0;
}

/* ── Alpha-beta fallback ─────────────────────────────────────────────────── */
#define SEARCH_INF 10000000

static const int PIECE_VAL[7] = {0, 100, 320, 330, 500, 900, 20000};

static const int pst_pawn[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0 };
static const int pst_knight[64] = {
  -50,-40,-30,-30,-30,-30,-40,-50,
  -40,-20,  0,  0,  0,  0,-20,-40,
  -30,  0, 10, 15, 15, 10,  0,-30,
  -30,  5, 15, 20, 20, 15,  5,-30,
  -30,  0, 15, 20, 20, 15,  0,-30,
  -30,  5, 10, 15, 15, 10,  5,-30,
  -40,-20,  0,  5,  5,  0,-20,-40,
  -50,-40,-30,-30,-30,-30,-40,-50 };
static const int pst_bishop[64] = {
  -20,-10,-10,-10,-10,-10,-10,-20,
  -10,  0,  0,  0,  0,  0,  0,-10,
  -10,  0,  5, 10, 10,  5,  0,-10,
  -10,  5,  5, 10, 10,  5,  5,-10,
  -10,  0, 10, 10, 10, 10,  0,-10,
  -10, 10, 10, 10, 10, 10, 10,-10,
  -10,  5,  0,  0,  0,  0,  5,-10,
  -20,-10,-10,-10,-10,-10,-10,-20 };
static const int pst_rook[64] = {
    0,  0,  0,  0,  0,  0,  0,  0,
    5, 10, 10, 10, 10, 10, 10,  5,
   -5,  0,  0,  0,  0,  0,  0, -5,
   -5,  0,  0,  0,  0,  0,  0, -5,
   -5,  0,  0,  0,  0,  0,  0, -5,
   -5,  0,  0,  0,  0,  0,  0, -5,
   -5,  0,  0,  0,  0,  0,  0, -5,
    0,  0,  0,  5,  5,  0,  0,  0 };
static const int pst_queen[64] = {
  -20,-10,-10, -5, -5,-10,-10,-20,
  -10,  0,  0,  0,  0,  0,  0,-10,
  -10,  0,  5,  5,  5,  5,  0,-10,
   -5,  0,  5,  5,  5,  5,  0, -5,
    0,  0,  5,  5,  5,  5,  0, -5,
  -10,  5,  5,  5,  5,  5,  0,-10,
  -10,  0,  5,  0,  0,  0,  0,-10,
  -20,-10,-10, -5, -5,-10,-10,-20 };
static const int pst_king[64] = {
  -30,-40,-40,-50,-50,-40,-40,-30,
  -30,-40,-40,-50,-50,-40,-40,-30,
  -30,-40,-40,-50,-50,-40,-40,-30,
  -30,-40,-40,-50,-50,-40,-40,-30,
  -20,-30,-30,-40,-40,-30,-30,-20,
  -10,-20,-20,-20,-20,-20,-20,-10,
   20, 20,  0,  0,  0,  0, 20, 20,
   20, 30, 10,  0,  0, 10, 30, 20 };

static int piece_idx(char u) {
    switch (u) {
        case 'P': return 1; case 'N': return 2; case 'B': return 3;
        case 'R': return 4; case 'Q': return 5; case 'K': return 6;
    } return 0;
}
static int mirror_sq(int sq) { return (7 - sq / 8) * 8 + sq % 8; }

static int evaluate(const Pos *p) {
    int score = 0;
    for (int i = 0; i < 64; i++) {
        char pc = p->b[i];
        if (pc == '.') continue;
        int w = is_white_piece(pc);
        char up = (char) toupper((unsigned char) pc);
        int sq = w ? i : mirror_sq(i), pst = 0;
        switch (up) {
            case 'P': pst = pst_pawn[sq];   break;
            case 'N': pst = pst_knight[sq]; break;
            case 'B': pst = pst_bishop[sq]; break;
            case 'R': pst = pst_rook[sq];   break;
            case 'Q': pst = pst_queen[sq];  break;
            case 'K': pst = pst_king[sq];   break;
        }
        if (w) score += PIECE_VAL[piece_idx(up)] + pst;
        else   score -= PIECE_VAL[piece_idx(up)] + pst;
    }
    return p->white_to_move ? score : -score;
}
static int mvv_lva(const Pos *p, Move m) {
    char v = p->b[m.to];
    if (v == '.') return 0;
    return PIECE_VAL[piece_idx((char) toupper((unsigned char) v))] * 10
          - PIECE_VAL[piece_idx((char) toupper((unsigned char) p->b[m.from]))];
}
static void sort_moves(const Pos *p, Move *ms, int n) {
    for (int i = 1; i < n; i++) {
        Move k = ms[i];
        int ks = mvv_lva(p, k);
        int j = i - 1;
        while (j >= 0 && mvv_lva(p, ms[j]) < ks) { ms[j + 1] = ms[j]; j--; }
        ms[j + 1] = k;
    }
}
static int alphabeta(const Pos *p, int depth, int alpha, int beta, int ply) {
    if (depth == 0) return evaluate(p);
    Move ms[256];
    int n = legal_moves(p, ms);
    if (n == 0) return in_check(p, p->white_to_move) ? -SEARCH_INF + ply : 0;
    sort_moves(p, ms, n);
    for (int i = 0; i < n; i++) {
        if (is_en_passant(p, ms[i])) continue;
        Pos np = make_move(p, ms[i]);
        int s = -alphabeta(&np, depth - 1, -beta, -alpha, ply + 1);
        if (s >= beta) return beta;
        if (s > alpha) alpha = s;
    }
    return alpha;
}
static Move fallback_best(const Pos *p, Move *ms, int n) {
    Move best = ms[0];
    int bs = -SEARCH_INF - 1;
    sort_moves(p, ms, n);
    for (int i = 0; i < n; i++) {
        if (is_en_passant(p, ms[i])) continue;
        if (is_repetition_move(p, ms[i])) continue;
        Pos np = make_move(p, ms[i]);
        int s = -alphabeta(&np, FALLBACK_DEPTH - 1, -SEARCH_INF, SEARCH_INF, 1);
        if (s > bs) { bs = s; best = ms[i]; }
    }
    if (bs == -SEARCH_INF - 1) {
        for (int i = 0; i < n; i++) {
            if (!is_en_passant(p, ms[i]) && !is_repetition_move(p, ms[i])) {
                best = ms[i];
                break;
            }
        }
    }
    return best;
}

/* ── lc0 subprocess ──────────────────────────────────────────────────────── */
static HANDLE g_lc0_stdin_write  = NULL;
static HANDLE g_lc0_stdout_read  = NULL;
static PROCESS_INFORMATION g_lc0_pi;
static int g_lc0_ok = 0;

static void lc0_write_line(const char *cmd) {
    if (!g_lc0_stdin_write) return;
    char buf[2048];
    int len = snprintf(buf, sizeof(buf), "%s\n", cmd);
    DWORD written;
    WriteFile(g_lc0_stdin_write, buf, (DWORD) len, &written, NULL);
}
static int lc0_read_line(char *buf, int sz) {
    // Read a chunk at a time instead of one byte per ReadFile call
    static char rbuf[4096];
    static int  rlen = 0;
    static int  rpos = 0;
    int i = 0;
    while (i < sz - 1) {
        if (rpos >= rlen) {
            DWORD got = 0;
            if (!ReadFile(g_lc0_stdout_read, rbuf, sizeof(rbuf), &got, NULL) || got == 0) return 0;
            rlen = (int)got;
            rpos = 0;
        }
        char c = rbuf[rpos++];
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = 0;
    return 1;
}
static int lc0_wait_for(const char *token, char *out, int outsz) {
    char line[1024];
    while (lc0_read_line(line, sizeof(line))) {
        if (strncmp(line, token, strlen(token)) == 0) {
            if (out) strncpy(out, line, outsz - 1);
            return 1;
        }
    }
    return 0;
}
static int init_lc0(void) {
    char lc0_path[MAX_PATH], weights_path[MAX_PATH];
    build_sibling_path(LC0_EXE_NAME,     lc0_path,     MAX_PATH);
    build_sibling_path(LC0_WEIGHTS_FILE, weights_path, MAX_PATH);

    if (GetFileAttributesA(lc0_path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[engine] lc0 not found: %s\n", lc0_path);
        return 0;
    }
    if (GetFileAttributesA(weights_path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[engine] weights not found: %s\n", weights_path);
        return 0;
    }

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE lc0_stdin_read = NULL, lc0_stdout_write = NULL;

    if (!CreatePipe(&lc0_stdin_read,   &g_lc0_stdin_write, &sa, 0) ||
        !CreatePipe(&g_lc0_stdout_read, &lc0_stdout_write,  &sa, 0)) {
        fprintf(stderr, "[engine] CreatePipe failed\n");
        return 0;
    }

    /* Parent-side handles must not be inherited by lc0 */
    SetHandleInformation(g_lc0_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_lc0_stdout_read, HANDLE_FLAG_INHERIT, 0);

    char cmd[MAX_PATH * 2 + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\" --weights=\"%s\" --backend=eigen --minibatch-size=1 --logfile= --verbose-move-stats=false", lc0_path, weights_path);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = lc0_stdin_read;
    si.hStdOutput = lc0_stdout_write;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    memset(&g_lc0_pi, 0, sizeof(g_lc0_pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_lc0_pi)) {
        fprintf(stderr, "[engine] CreateProcess failed (error %lu)\n", GetLastError());
        CloseHandle(lc0_stdin_read);
        CloseHandle(lc0_stdout_write);
        return 0;
    }

    CloseHandle(lc0_stdin_read);
    CloseHandle(lc0_stdout_write);

    lc0_write_line("uci");
    if (!lc0_wait_for("uciok", NULL, 0)) {
        fprintf(stderr, "[engine] lc0 uci handshake failed\n");
        return 0;
    }
    lc0_write_line("isready");
    if (!lc0_wait_for("readyok", NULL, 0)) {
        fprintf(stderr, "[engine] lc0 isready failed\n");
        return 0;
    }
    return 1;
}
static void shutdown_lc0(void) {
    if (g_lc0_stdin_write) {
        lc0_write_line("quit");
        CloseHandle(g_lc0_stdin_write);
        g_lc0_stdin_write = NULL;
    }
    if (g_lc0_stdout_read) {
        CloseHandle(g_lc0_stdout_read);
        g_lc0_stdout_read = NULL;
    }
    if (g_lc0_pi.hProcess) {
        WaitForSingleObject(g_lc0_pi.hProcess, 3000);
        CloseHandle(g_lc0_pi.hProcess);
        CloseHandle(g_lc0_pi.hThread);
    }
}

/* ── Query lc0 for best non-EP, non-repetition move ─────────────────────── */
static int query_lc0(const Pos *p, const char *pos_cmd, char best_move[6]) {
    lc0_write_line(pos_cmd);
    char go_cmd[64];
    snprintf(go_cmd, sizeof(go_cmd), "go movetime %d", LC0_MOVETIME_MS);
    lc0_write_line(go_cmd);

    char line[1024];
    while (lc0_read_line(line, sizeof(line))) {
        if (strncmp(line, "bestmove", 8) == 0) {
            char *mv = line + 9;
            int i = 0;
            while (*mv && *mv != ' ' && i < 5) best_move[i++] = *mv++;
            best_move[i] = 0;
            if (strlen(best_move) < 4) return 0;
            Move m;
            m.from  = sq_index(best_move);
            m.to    = sq_index(best_move + 2);
            m.promo = (strlen(best_move) >= 5) ? best_move[4] : 0;
            if (is_en_passant(p, m) || is_repetition_move(p, m)) return 0;
            return 1;
        }
    }
    return 0;
}

/* ── UCI helpers ─────────────────────────────────────────────────────────── */
static char g_pos_cmd[2048] = {0};

static void apply_uci_move(Pos *p, const char *uci) {
    if (!uci || strlen(uci) < 4) return;
    Move m;
    m.from  = sq_index(uci);
    m.to    = sq_index(uci + 2);
    m.promo = (strlen(uci) >= 5) ? uci[4] : 0;
    Pos np = make_move(p, m);
    *p = np;
}

static void rebuild_pos_cmd(void) {
    if (g_pos_is_startpos) {
        strncpy(g_pos_cmd, "position startpos", sizeof(g_pos_cmd) - 1);
    } else {
        snprintf(g_pos_cmd, sizeof(g_pos_cmd), "position fen %s", g_pos_fen);
    }
    if (g_move_history_len > 0) {
        strncat(g_pos_cmd, " moves", sizeof(g_pos_cmd) - strlen(g_pos_cmd) - 1);
        for (int i = 0; i < g_move_history_len; i++) {
            strncat(g_pos_cmd, " ",               sizeof(g_pos_cmd) - strlen(g_pos_cmd) - 1);
            strncat(g_pos_cmd, g_move_history[i], sizeof(g_pos_cmd) - strlen(g_pos_cmd) - 1);
        }
    }
}

static void parse_position(Pos *p, const char *line) {
    // position startpos [moves ...]
    // position fen <6 fields> [moves ...]
    char buf[8192];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *toks[1024];
    int nt = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok && nt < 1024; tok = strtok_r(NULL, " \t\r\n", &save)) {
        toks[nt++] = tok;
    }

    int i = 1;
    if (i < nt && strcmp(toks[i], "startpos") == 0) {
        pos_start(p);
        g_pos_is_startpos = 1;
        memset(g_pos_fen, 0, sizeof(g_pos_fen));
        i++;
    } else if (i < nt && strcmp(toks[i], "fen") == 0) {
        i++;
        char fen[512] = {0};
        for (int k = 0; k < 6 && i < nt; k++, i++) {
            if (k)
                strcat(fen, " ");
            strcat(fen, toks[i]);
        }
        pos_from_fen(p, fen);
        g_pos_is_startpos = 0;
        strncpy(g_pos_fen, fen, sizeof(g_pos_fen) - 1);
    }

    /* Rebuild full position history and move string history */
    g_history_len = 0;
    g_move_history_len = 0;
    g_halfmove_clock = 0;
    BoardSnap snap;
    memcpy(snap.b, p->b, 64);
    snap.white_to_move = p->white_to_move;
    if (g_history_len < MAX_HISTORY)
        g_pos_history[g_history_len++] = snap;

    if (i < nt && strcmp(toks[i], "moves") == 0) {
        i++;
        for (; i < nt; i++) {
            // check if this move resets the halfmove clock (pawn move or capture)
            int from = sq_index(toks[i]);
            char piece = p->b[from];
            char target = p->b[sq_index(toks[i] + 2)];
            if (toupper((unsigned char)piece) == 'P' || target != '.')
                g_halfmove_clock = 0;
            else
                g_halfmove_clock++;
            apply_uci_move(p, toks[i]);
            memcpy(snap.b, p->b, 64);
            snap.white_to_move = p->white_to_move;
            if (g_history_len < MAX_HISTORY)
                g_pos_history[g_history_len++] = snap;
            if (g_move_history_len < MAX_HISTORY) {
                strncpy(g_move_history[g_move_history_len], toks[i], 5);
                g_move_history[g_move_history_len][5] = 0;
                g_move_history_len++;
            }
        }
    }

    rebuild_pos_cmd();
}

static void print_bestmove(Move m) {
    char a[3], b[3];
    index_to_sq(m.from, a);
    index_to_sq(m.to, b);
    if (m.promo) printf("bestmove %s%s%c\n", a, b, m.promo);
    else printf("bestmove %s%s\n", a, b);
    fflush(stdout);
}

int main(void) {
    get_exe_dir();

    Pos pos;
    pos_start(&pos);

    g_lc0_ok = init_lc0();
    if (!g_lc0_ok)
        fprintf(stderr, "[engine] lc0 unavailable — using alpha-beta fallback\n");

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;

        if (strcmp(line, "uci") == 0) {
            printf("id name lc0_engine\n");
            printf("id author CSharpestTools\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(line, "ucinewgame") == 0) {
            pos_start(&pos);
            if (g_lc0_ok) lc0_write_line("ucinewgame");
        } else if (strncmp(line, "position", 8) == 0) {
            parse_position(&pos, line);
        } else if (strncmp(line, "go", 2) == 0) {
            // check all draw conditions before searching for a move
            if (is_threefold_draw()) {
                printf("bestmove 0000\n");
                fprintf(stderr, "[engine] draw: threefold repetition\n");
                fflush(stdout);
                continue;
            }
            if (g_halfmove_clock >= 100) {
                printf("bestmove 0000\n");
                fprintf(stderr, "[engine] draw: fifty-move rule\n");
                fflush(stdout);
                continue;
            }
            if (is_insufficient_material(&pos)) {
                printf("bestmove 0000\n");
                fprintf(stderr, "[engine] draw: insufficient material\n");
                fflush(stdout);
                continue;
            }
            Move ms[256];
            int n = legal_moves(&pos, ms);
            if (n <= 0) {
                if (!in_check(&pos, pos.white_to_move)) {
                    fprintf(stderr, "[engine] draw: stalemate\n");
                }
                printf("bestmove 0000\n");
                fflush(stdout);
            } else {
                char uci_move[6] = {0};
                int played = 0;
                if (g_lc0_ok && query_lc0(&pos, g_pos_cmd, uci_move)) {
                    for (int i = 0; i < n; i++) {
                        char legal_uci[6] = {0};
                        char a[3], b[3];
                        index_to_sq(ms[i].from, a);
                        index_to_sq(ms[i].to,   b);
                        if (ms[i].promo)
                            snprintf(legal_uci, sizeof(legal_uci), "%s%s%c", a, b, ms[i].promo);
                        else
                            snprintf(legal_uci, sizeof(legal_uci), "%s%s", a, b);
                        if (strcmp(legal_uci, uci_move) == 0) {
                            fprintf(stderr, "[engine] lc0: %s\n", uci_move);
                            print_bestmove(ms[i]);
                            played = 1;
                            break;
                        }
                    }
                }
                if (!played) {
                    Move best = fallback_best(&pos, ms, n);
                    char a[3], b[3];
                    index_to_sq(best.from, a);
                    index_to_sq(best.to, b);
                    fprintf(stderr, "[engine] fallback: %s%s\n", a, b);
                    print_bestmove(best);
                }
            }
        } else if (strcmp(line, "quit") == 0) {
            break;
        }
    }

    shutdown_lc0();
    return 0;
}
