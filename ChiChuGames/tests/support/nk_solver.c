#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NKS_SIZE 8
#define NKS_CELLS (NKS_SIZE * NKS_SIZE)
#define NKS_UNKNOWN 0
#define NKS_WHITE 1
#define NKS_BLACK 2

typedef struct {
    uint8_t cell[NKS_CELLS];
    uint8_t clue[NKS_CELLS];
    int limit;
    int solutions;
    long *nodes;
} nks_t;

static const int nks_dx[4] = {1, -1, 0, 0};
static const int nks_dy[4] = {0, 0, 1, -1};

static bool nks_inside(int x, int y)
{
    return x >= 0 && x < NKS_SIZE && y >= 0 && y < NKS_SIZE;
}

static bool nks_no_black_square(const nks_t *solver)
{
    for (int y = 0; y < NKS_SIZE - 1; y++)
        for (int x = 0; x < NKS_SIZE - 1; x++) {
            int i = y * NKS_SIZE + x;
            if (solver->cell[i] == NKS_BLACK &&
                solver->cell[i + 1] == NKS_BLACK &&
                solver->cell[i + NKS_SIZE] == NKS_BLACK &&
                solver->cell[i + NKS_SIZE + 1] == NKS_BLACK)
                return false;
        }
    return true;
}

static bool nks_white_components_valid(const nks_t *solver, bool complete)
{
    uint8_t seen[NKS_CELLS] = {0};
    int queue[NKS_CELLS];

    for (int start = 0; start < NKS_CELLS; start++) {
        if (solver->cell[start] != NKS_WHITE || seen[start]) continue;
        int head = 0, tail = 0, size = 0, clues = 0, target = 0;
        bool expandable = false;
        queue[tail++] = start;
        seen[start] = 1;
        while (head < tail) {
            int cell = queue[head++];
            int x = cell % NKS_SIZE, y = cell / NKS_SIZE;
            size++;
            if (solver->clue[cell]) {
                clues++;
                target = solver->clue[cell];
            }
            for (int direction = 0; direction < 4; direction++) {
                int nx = x + nks_dx[direction], ny = y + nks_dy[direction];
                int next;
                if (!nks_inside(nx, ny)) continue;
                next = ny * NKS_SIZE + nx;
                if (solver->cell[next] == NKS_UNKNOWN) expandable = true;
                if (solver->cell[next] == NKS_WHITE && !seen[next]) {
                    seen[next] = 1;
                    queue[tail++] = next;
                }
            }
        }
        if (clues > 1 || (clues == 1 && size > target)) return false;
        if (complete && (clues != 1 || size != target)) return false;
        if (!complete && !expandable && (clues != 1 || size != target)) return false;
    }
    return true;
}

static bool nks_each_clue_can_reach_size(const nks_t *solver)
{
    int queue[NKS_CELLS];
    uint8_t seen[NKS_CELLS];

    for (int start = 0; start < NKS_CELLS; start++) {
        if (!solver->clue[start]) continue;
        int head = 0, tail = 0;
        memset(seen, 0, sizeof(seen));
        queue[tail++] = start;
        seen[start] = 1;
        while (head < tail) {
            int cell = queue[head++];
            int x = cell % NKS_SIZE, y = cell / NKS_SIZE;
            for (int direction = 0; direction < 4; direction++) {
                int nx = x + nks_dx[direction], ny = y + nks_dy[direction];
                int next;
                if (!nks_inside(nx, ny)) continue;
                next = ny * NKS_SIZE + nx;
                if (solver->cell[next] != NKS_BLACK && !seen[next]) {
                    seen[next] = 1;
                    queue[tail++] = next;
                }
            }
        }
        if (tail < solver->clue[start]) return false;
    }
    return true;
}

static bool nks_black_can_connect(const nks_t *solver, bool complete)
{
    int first = -1, black_count = 0;
    int queue[NKS_CELLS];
    uint8_t seen[NKS_CELLS] = {0};

    for (int i = 0; i < NKS_CELLS; i++)
        if (solver->cell[i] == NKS_BLACK) {
            if (first < 0) first = i;
            black_count++;
        }
    if (first < 0) return !complete;
    int head = 0, tail = 0, reached_black = 0;
    queue[tail++] = first;
    seen[first] = 1;
    while (head < tail) {
        int cell = queue[head++];
        int x = cell % NKS_SIZE, y = cell / NKS_SIZE;
        if (solver->cell[cell] == NKS_BLACK) reached_black++;
        for (int direction = 0; direction < 4; direction++) {
            int nx = x + nks_dx[direction], ny = y + nks_dy[direction];
            int next;
            if (!nks_inside(nx, ny)) continue;
            next = ny * NKS_SIZE + nx;
            if (!seen[next] && solver->cell[next] != NKS_WHITE) {
                seen[next] = 1;
                queue[tail++] = next;
            }
        }
    }
    return reached_black == black_count && (!complete || tail == black_count);
}

static bool nks_valid(const nks_t *solver, bool complete)
{
    return nks_no_black_square(solver) &&
           nks_white_components_valid(solver, complete) &&
           nks_each_clue_can_reach_size(solver) &&
           nks_black_can_connect(solver, complete);
}

static int nks_choose_cell(const nks_t *solver)
{
    int chosen = -1, best_score = -1;

    for (int cell = 0; cell < NKS_CELLS; cell++) {
        if (solver->cell[cell] != NKS_UNKNOWN) continue;
        int x = cell % NKS_SIZE, y = cell / NKS_SIZE;
        int score = 0;
        for (int direction = 0; direction < 4; direction++) {
            int nx = x + nks_dx[direction], ny = y + nks_dy[direction];
            if (nks_inside(nx, ny) &&
                solver->cell[ny * NKS_SIZE + nx] != NKS_UNKNOWN)
                score += 2;
        }
        if (score > best_score) {
            best_score = score;
            chosen = cell;
        }
    }
    return chosen;
}

static void nks_search(nks_t *solver)
{
    int cell;

    if (solver->solutions >= solver->limit || !nks_valid(solver, false)) return;
    if (solver->nodes) (*solver->nodes)++;
    cell = nks_choose_cell(solver);
    if (cell < 0) {
        if (nks_valid(solver, true)) solver->solutions++;
        return;
    }
    solver->cell[cell] = NKS_BLACK;
    nks_search(solver);
    if (solver->solutions < solver->limit) {
        solver->cell[cell] = NKS_WHITE;
        nks_search(solver);
    }
    solver->cell[cell] = NKS_UNKNOWN;
}

static int nks_solve(nks_t *solver, const char *rows[NKS_SIZE], int limit, long *nodes)
{
    memset(solver, 0, sizeof(*solver));
    solver->limit = limit > 0 ? limit : 1;
    solver->nodes = nodes;
    if (nodes) *nodes = 0;
    for (int y = 0; y < NKS_SIZE; y++)
        for (int x = 0; x < NKS_SIZE; x++) {
            int cell = y * NKS_SIZE + x;
            char value = rows[y][x];
            if (value >= '1' && value <= '9') {
                solver->cell[cell] = NKS_WHITE;
                solver->clue[cell] = (uint8_t)(value - '0');
            }
        }
    nks_search(solver);
    return solver->solutions;
}
