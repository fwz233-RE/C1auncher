/* 游戏描述表 — 菜单顺序与框架契约 */
#include "game.h"
#include <stddef.h>

/* 菜单分类名 */
const char *const g_cat_names[CAT_COUNT] = {
    "LOGIC", "BOARD", "CARDS", "WORDS", "NUMBERS",
    "DICE", "QUIZ", "SIM", "ACTION",
};

void snake_enter(void);
void snake_exit(void);
void snake_tick(uint64_t now);
void snake_render(void);
void snake_on_key(const key_event_t *ev);

void tetris_enter(void);  void tetris_exit(void);
void tetris_tick(uint64_t now); void tetris_render(void);
void tetris_on_key(const key_event_t *ev);

void g2048_enter(void);   void g2048_exit(void);
void g2048_tick(uint64_t now); void g2048_render(void);
void g2048_on_key(const key_event_t *ev);

void mines_enter(void);   void mines_exit(void);
void mines_tick(uint64_t now); void mines_render(void);
void mines_on_key(const key_event_t *ev);

void sokoban_enter(void); void sokoban_exit(void);
void sokoban_tick(uint64_t now); void sokoban_render(void);
void sokoban_on_key(const key_event_t *ev);

void memory_enter(void);  void memory_exit(void);
void memory_tick(uint64_t now); void memory_render(void);
void memory_on_key(const key_event_t *ev);

void sudoku_enter(void);  void sudoku_exit(void);
void sudoku_tick(uint64_t now); void sudoku_render(void);
void sudoku_on_key(const key_event_t *ev);

void reversi_enter(void);  void reversi_exit(void);
void reversi_tick(uint64_t now); void reversi_render(void);
void reversi_on_key(const key_event_t *ev);

void fifteen_enter(void);  void fifteen_exit(void);
void fifteen_tick(uint64_t now); void fifteen_render(void);
void fifteen_on_key(const key_event_t *ev);

void lightsout_enter(void);  void lightsout_exit(void);
void lightsout_tick(uint64_t now); void lightsout_render(void);
void lightsout_on_key(const key_event_t *ev);

void wordle_enter(void);  void wordle_exit(void);
void wordle_tick(uint64_t now); void wordle_render(void);
void wordle_on_key(const key_event_t *ev);

void game24_enter(void);  void game24_exit(void);
void game24_tick(uint64_t now); void game24_render(void);
void game24_on_key(const key_event_t *ev);

void klotski_enter(void);  void klotski_exit(void);
void klotski_tick(uint64_t now); void klotski_render(void);
void klotski_on_key(const key_event_t *ev);

void maze_enter(void);  void maze_exit(void);
void maze_tick(uint64_t now); void maze_render(void);
void maze_on_key(const key_event_t *ev);

void peg_enter(void);  void peg_exit(void);
void peg_tick(uint64_t now); void peg_render(void);
void peg_on_key(const key_event_t *ev);

void connect4_enter(void);  void connect4_exit(void);
void connect4_tick(uint64_t now); void connect4_render(void);
void connect4_on_key(const key_event_t *ev);

void life_enter(void);  void life_exit(void);
void life_tick(uint64_t now); void life_render(void);
void life_on_key(const key_event_t *ev);

void mastermind_enter(void);  void mastermind_exit(void);
void mastermind_tick(uint64_t now); void mastermind_render(void);
void mastermind_on_key(const key_event_t *ev);

void hangman_enter(void);  void hangman_exit(void);
void hangman_tick(uint64_t now); void hangman_render(void);
void hangman_on_key(const key_event_t *ev);

void nonogram_enter(void);  void nonogram_exit(void);
void nonogram_tick(uint64_t now); void nonogram_render(void);
void nonogram_on_key(const key_event_t *ev);

void bullscows_enter(void);  void bullscows_exit(void);
void bullscows_tick(uint64_t now); void bullscows_render(void);
void bullscows_on_key(const key_event_t *ev);

void dotsbox_enter(void);  void dotsbox_exit(void);
void dotsbox_tick(uint64_t now); void dotsbox_render(void);
void dotsbox_on_key(const key_event_t *ev);

void blackjack_enter(void);  void blackjack_exit(void);
void blackjack_tick(uint64_t now); void blackjack_render(void);
void blackjack_on_key(const key_event_t *ev);

void pyramid_enter(void);  void pyramid_exit(void);
void pyramid_tick(uint64_t now); void pyramid_render(void);
void pyramid_on_key(const key_event_t *ev);

void golf_enter(void);  void golf_exit(void);
void golf_tick(uint64_t now); void golf_render(void);
void golf_on_key(const key_event_t *ev);

void klondike_enter(void);  void klondike_exit(void);
void klondike_tick(uint64_t now); void klondike_render(void);
void klondike_on_key(const key_event_t *ev);

void freecell_enter(void);  void freecell_exit(void);
void freecell_tick(uint64_t now); void freecell_render(void);
void freecell_on_key(const key_event_t *ev);

void whack_enter(void);  void whack_exit(void);
void whack_tick(uint64_t now); void whack_render(void);
void whack_on_key(const key_event_t *ev);

void kakuro_enter(void);  void kakuro_exit(void);
void kakuro_tick(uint64_t now); void kakuro_render(void);
void kakuro_on_key(const key_event_t *ev);

void nim_enter(void);  void nim_exit(void);
void nim_tick(uint64_t now); void nim_render(void);
void nim_on_key(const key_event_t *ev);

void battleship_enter(void);  void battleship_exit(void);
void battleship_tick(uint64_t now); void battleship_render(void);
void battleship_on_key(const key_event_t *ev);

void yahtzee_enter(void);  void yahtzee_exit(void);
void yahtzee_tick(uint64_t now); void yahtzee_render(void);
void yahtzee_on_key(const key_event_t *ev);

void hex_enter(void);  void hex_exit(void);
void hex_tick(uint64_t now); void hex_render(void);
void hex_on_key(const key_event_t *ev);

void hexmines_enter(void);  void hexmines_exit(void);
void hexmines_tick(uint64_t now); void hexmines_render(void);
void hexmines_on_key(const key_event_t *ev);

void checkers_enter(void);  void checkers_exit(void);
void checkers_tick(uint64_t now); void checkers_render(void);
void checkers_on_key(const key_event_t *ev);

void isola_enter(void);  void isola_exit(void);
void isola_tick(uint64_t now); void isola_render(void);
void isola_on_key(const key_event_t *ev);

void chess_enter(void);  void chess_exit(void);
void chess_tick(uint64_t now); void chess_render(void);
void chess_on_key(const key_event_t *ev);

void darkchess_enter(void);  void darkchess_exit(void);
void darkchess_tick(uint64_t now); void darkchess_render(void);
void darkchess_on_key(const key_event_t *ev);

void binarypuzzle_enter(void);  void binarypuzzle_exit(void);
void binarypuzzle_tick(uint64_t now); void binarypuzzle_render(void);
void binarypuzzle_on_key(const key_event_t *ev);

void futoshiki_enter(void);  void futoshiki_exit(void);
void futoshiki_tick(uint64_t now); void futoshiki_render(void);
void futoshiki_on_key(const key_event_t *ev);

void skyscrapers_enter(void);  void skyscrapers_exit(void);
void skyscrapers_tick(uint64_t now); void skyscrapers_render(void);
void skyscrapers_on_key(const key_event_t *ev);

void numberlink_enter(void);  void numberlink_exit(void);
void numberlink_tick(uint64_t now); void numberlink_render(void);
void numberlink_on_key(const key_event_t *ev);

void anagrams_enter(void);  void anagrams_exit(void);
void anagrams_tick(uint64_t now); void anagrams_render(void);
void anagrams_on_key(const key_event_t *ev);

void questions_enter(void);  void questions_exit(void);
void questions_tick(uint64_t now); void questions_render(void);
void questions_on_key(const key_event_t *ev);

void killersudoku_enter(void);  void killersudoku_exit(void);
void killersudoku_tick(uint64_t now); void killersudoku_render(void);
void killersudoku_on_key(const key_event_t *ev);

void nurikabe_enter(void);  void nurikabe_exit(void);
void nurikabe_tick(uint64_t now); void nurikabe_render(void);
void nurikabe_on_key(const key_event_t *ev);

void pentomino_enter(void);  void pentomino_exit(void);
void pentomino_tick(uint64_t now); void pentomino_render(void);
void pentomino_on_key(const key_event_t *ev);

void matchstick_enter(void);  void matchstick_exit(void);
void matchstick_tick(uint64_t now); void matchstick_render(void);
void matchstick_on_key(const key_event_t *ev);

void simon_enter(void);  void simon_exit(void);
void simon_tick(uint64_t now); void simon_render(void);
void simon_on_key(const key_event_t *ev);

void spider_enter(void);  void spider_exit(void);
void spider_tick(uint64_t now); void spider_render(void);
void spider_on_key(const key_event_t *ev);

void pokerdraw_enter(void);  void pokerdraw_exit(void);
void pokerdraw_tick(uint64_t now); void pokerdraw_render(void);
void pokerdraw_on_key(const key_event_t *ev);

void towerdefense_enter(void);  void towerdefense_exit(void);
void towerdefense_tick(uint64_t now); void towerdefense_render(void);
void towerdefense_on_key(const key_event_t *ev);

void ccheckers_enter(void);  void ccheckers_exit(void);
void ccheckers_tick(uint64_t now); void ccheckers_render(void);
void ccheckers_on_key(const key_event_t *ev);

void backgammon_enter(void);  void backgammon_exit(void);
void backgammon_tick(uint64_t now); void backgammon_render(void);
void backgammon_on_key(const key_event_t *ev);

void boggle_enter(void);  void boggle_exit(void);
void boggle_tick(uint64_t now); void boggle_render(void);
void boggle_on_key(const key_event_t *ev);

void wordchain_enter(void);  void wordchain_exit(void);
void wordchain_tick(uint64_t now); void wordchain_render(void);
void wordchain_on_key(const key_event_t *ev);

void mathtrain_enter(void);  void mathtrain_exit(void);
void mathtrain_tick(uint64_t now); void mathtrain_render(void);
void mathtrain_on_key(const key_event_t *ev);

void primerush_enter(void);  void primerush_exit(void);
void primerush_tick(uint64_t now); void primerush_render(void);
void primerush_on_key(const key_event_t *ev);

void binmorse_enter(void);  void binmorse_exit(void);
void binmorse_tick(uint64_t now); void binmorse_render(void);
void binmorse_on_key(const key_event_t *ev);

void tictacdice_enter(void);  void tictacdice_exit(void);
void tictacdice_tick(uint64_t now); void tictacdice_render(void);
void tictacdice_on_key(const key_event_t *ev);

void slitherlink_enter(void);  void slitherlink_exit(void);
void slitherlink_tick(uint64_t now); void slitherlink_render(void);
void slitherlink_on_key(const key_event_t *ev);

void hashi_enter(void);  void hashi_exit(void);
void hashi_tick(uint64_t now); void hashi_render(void);
void hashi_on_key(const key_event_t *ev);

void starbattle_enter(void);  void starbattle_exit(void);
void starbattle_tick(uint64_t now); void starbattle_render(void);
void starbattle_on_key(const key_event_t *ev);

void tents_enter(void);  void tents_exit(void);
void tents_tick(uint64_t now); void tents_render(void);
void tents_on_key(const key_event_t *ev);

void kenken_enter(void);  void kenken_exit(void);
void kenken_tick(uint64_t now); void kenken_render(void);
void kenken_on_key(const key_event_t *ev);

void jigsaw_enter(void);  void jigsaw_exit(void);
void jigsaw_tick(uint64_t now); void jigsaw_render(void);
void jigsaw_on_key(const key_event_t *ev);

void life2_enter(void);  void life2_exit(void);
void life2_tick(uint64_t now); void life2_render(void);
void life2_on_key(const key_event_t *ev);

void memdigits_enter(void);  void memdigits_exit(void);
void memdigits_tick(uint64_t now); void memdigits_render(void);
void memdigits_on_key(const key_event_t *ev);

void geo_enter(void);  void geo_exit(void);
void geo_tick(uint64_t now); void geo_render(void);
void geo_on_key(const key_event_t *ev);

void flagquiz_enter(void);  void flagquiz_exit(void);
void flagquiz_tick(uint64_t now); void flagquiz_render(void);
void flagquiz_on_key(const key_event_t *ev);

void elements_enter(void);  void elements_exit(void);
void elements_tick(uint64_t now); void elements_render(void);
void elements_on_key(const key_event_t *ev);

void timestrain_enter(void);  void timestrain_exit(void);
void timestrain_tick(uint64_t now); void timestrain_render(void);
void timestrain_on_key(const key_event_t *ev);

void clockquiz_enter(void);  void clockquiz_exit(void);
void clockquiz_tick(uint64_t now); void clockquiz_render(void);
void clockquiz_on_key(const key_event_t *ev);

void riddle_enter(void);  void riddle_exit(void);
void riddle_tick(uint64_t now); void riddle_render(void);
void riddle_on_key(const key_event_t *ev);

void dicequest_enter(void);  void dicequest_exit(void);
void dicequest_tick(uint64_t now); void dicequest_render(void);
void dicequest_on_key(const key_event_t *ev);

void liarsdice_enter(void);  void liarsdice_exit(void);
void liarsdice_tick(uint64_t now); void liarsdice_render(void);
void liarsdice_on_key(const key_event_t *ev);

void miner_enter(void);  void miner_exit(void);
void miner_tick(uint64_t now); void miner_render(void);
void miner_on_key(const key_event_t *ev);

void rails_enter(void);  void rails_exit(void);
void rails_tick(uint64_t now); void rails_render(void);
void rails_on_key(const key_event_t *ev);

void citybuilder_enter(void);  void citybuilder_exit(void);
void citybuilder_tick(uint64_t now); void citybuilder_render(void);
void citybuilder_on_key(const key_event_t *ev);

void beefarm_enter(void);  void beefarm_exit(void);
void beefarm_tick(uint64_t now); void beefarm_render(void);
void beefarm_on_key(const key_event_t *ev);

void mushgarden_enter(void);  void mushgarden_exit(void);
void mushgarden_tick(uint64_t now); void mushgarden_render(void);
void mushgarden_on_key(const key_event_t *ev);

void quake_enter(void);  void quake_exit(void);
void quake_tick(uint64_t now); void quake_render(void);
void quake_on_key(const key_event_t *ev);

void antcolony_enter(void);  void antcolony_exit(void);
void antcolony_tick(uint64_t now); void antcolony_render(void);
void antcolony_on_key(const key_event_t *ev);

void mahjongmatch_enter(void);  void mahjongmatch_exit(void);
void mahjongmatch_tick(uint64_t now); void mahjongmatch_render(void);
void mahjongmatch_on_key(const key_event_t *ev);

void gomoku_enter(void);  void gomoku_exit(void);
void gomoku_tick(uint64_t now); void gomoku_render(void);
void gomoku_on_key(const key_event_t *ev);

void hanoi_enter(void);   void hanoi_exit(void);
void hanoi_tick(uint64_t now); void hanoi_render(void);
void hanoi_on_key(const key_event_t *ev);

const game_desc_t g_games[GAME_COUNT] = {
    [G_SNAKE] = {
        .cat = CAT_ACTION,
        .id = G_SNAKE, .title = "SNAKE", .tagline = "EAT & GROW",
        .help = { "SNAKE", "ARROWS: MOVE", "P: PAUSE  N: NEW", "OK/N: RETRY  BACK: QUIT", NULL },
        .enter = snake_enter, .exit = snake_exit,
        .tick = snake_tick, .render = snake_render, .on_key = snake_on_key,
        .tick_interval_ms = 700,
    },
    [G_TETRIS] = {
        .cat = CAT_ACTION,
        .id = G_TETRIS, .title = "TETRIS", .tagline = "FALLING BLOCKS",
        .help = { "TETRIS", "LT/RT: MOVE  UP: ROTATE", "DN: SOFT  OK: HARD DROP", "P: PAUSE  N: NEW", NULL },
        .enter = tetris_enter, .exit = tetris_exit,
        .tick = tetris_tick, .render = tetris_render, .on_key = tetris_on_key,
        .tick_interval_ms = 700,
        .repeat_init_ms = 200, .repeat_ms = 60,
    },
    [G_2048] = {
        .cat = CAT_ACTION,
        .id = G_2048, .title = "2048", .tagline = "MERGE TILES",
        .help = { "2048", "ARROWS/WASD: SLIDE", "MERGE EQUAL TILES TO 2048", "PATTERNS = VALUES (SEE LEGEND)", NULL },
        .enter = g2048_enter, .exit = g2048_exit,
        .tick = g2048_tick, .render = g2048_render, .on_key = g2048_on_key,
        .tick_interval_ms = 0,
    },
    [G_MINES] = {
        .cat = CAT_ACTION,
        .id = G_MINES, .title = "MINESWEEPER", .tagline = "CLEAR THE FIELD",
        .help = { "MINESWEEPER", "ARROWS/WASD: MOVE CURSOR", "OK: REVEAL  F/DEL: FLAG", "FIRST REVEAL IS ALWAYS SAFE", NULL },
        .enter = mines_enter, .exit = mines_exit,
        .tick = mines_tick, .render = mines_render, .on_key = mines_on_key,
        .tick_interval_ms = 0,
    },
    [G_SOKOBAN] = {
        .cat = CAT_ACTION,
        .id = G_SOKOBAN, .title = "SOKOBAN", .tagline = "BOX PUSHER",
        .help = { "SOKOBAN", "ARROWS/WASD: MOVE/PUSH", "U: UNDO  R: RESET", "PUT ALL BOXES ON DOTS", "8 RANDOM DIFFICULTIES", NULL },
        .enter = sokoban_enter, .exit = sokoban_exit,
        .tick = sokoban_tick, .render = sokoban_render, .on_key = sokoban_on_key,
        .tick_interval_ms = 0,
    },
    [G_MEMORY] = {
        .cat = CAT_ACTION,
        .id = G_MEMORY, .title = "MEMORY", .tagline = "PAIR MATCH",
        .help = { "MEMORY", "ARROWS/WASD: MOVE  OK: FLIP", "MATCH ALL 12 PAIRS", "FEWER MOVES = BETTER", NULL },
        .enter = memory_enter, .exit = memory_exit,
        .tick = memory_tick, .render = memory_render, .on_key = memory_on_key,
        .tick_interval_ms = 100,
    },
    [G_SUDOKU] = {
        .cat = CAT_LOGIC,
        .id = G_SUDOKU, .title = "SUDOKU", .tagline = "NUMBER PLACE",
        .help = { "SUDOKU", "ARROWS/WASD: MOVE  OK: SELECT", "1-9: FILL  DEL: CLEAR", "P: CANDIDATE MODE  N: NEXT", NULL },
        .enter = sudoku_enter, .exit = sudoku_exit,
        .tick = sudoku_tick, .render = sudoku_render, .on_key = sudoku_on_key,
        .tick_interval_ms = 0,
    },
    [G_REVERSI] = {
        .cat = CAT_BOARD,
        .id = G_REVERSI, .title = "REVERSI", .tagline = "FLIP THE BOARD",
        .help = { "REVERSI", "ARROWS/WASD: MOVE  OK: PLACE", "YOU: BLACK  AI: WHITE", "MORE DISKS WINS", NULL },
        .enter = reversi_enter, .exit = reversi_exit,
        .tick = reversi_tick, .render = reversi_render, .on_key = reversi_on_key,
        .tick_interval_ms = 0,
    },
    [G_FIFTEEN] = {
        .cat = CAT_LOGIC,
        .id = G_FIFTEEN, .title = "FIFTEEN", .tagline = "SLIDE PUZZLE",
        .help = { "FIFTEEN", "ARROWS/WASD: SLIDE TILES", "ARRANGE 1-15 IN ORDER", "N: NEW SHUFFLE", NULL },
        .enter = fifteen_enter, .exit = fifteen_exit,
        .tick = fifteen_tick, .render = fifteen_render, .on_key = fifteen_on_key,
        .tick_interval_ms = 0,
    },
    [G_LIGHTSOUT] = {
        .cat = CAT_LOGIC,
        .id = G_LIGHTSOUT, .title = "LIGHTS OUT", .tagline = "TURN THEM OFF",
        .help = { "LIGHTS OUT", "ARROWS/WASD: MOVE  OK: PRESS", "PRESS TOGGLES CROSS", "TURN ALL LIGHTS OFF", NULL },
        .enter = lightsout_enter, .exit = lightsout_exit,
        .tick = lightsout_tick, .render = lightsout_render, .on_key = lightsout_on_key,
        .tick_interval_ms = 0,
    },
    [G_WORDLE] = {
        .cat = CAT_WORDS,
        .id = G_WORDLE, .title = "WORDLE", .tagline = "GUESS THE WORD",
        .help = { "WORDLE", "TYPE 5 LETTERS, OK SUBMITS", "DEL ERASES", "BLACK: RIGHT SPOT", "SLASH: WRONG SPOT", NULL },
        .enter = wordle_enter, .exit = wordle_exit,
        .tick = wordle_tick, .render = wordle_render, .on_key = wordle_on_key,
        .tick_interval_ms = 700,
    },
    [G_24GAME] = {
        .cat = CAT_NUMBERS,
        .id = G_24GAME, .title = "24 GAME", .tagline = "MAKE 24",
        .help = { "24 GAME", "PICK 2 CARDS + OPERATOR", "MERGE TO REACH 24", "X: CLEAR SELECT  N: NEW", NULL },
        .enter = game24_enter, .exit = game24_exit,
        .tick = game24_tick, .render = game24_render, .on_key = game24_on_key,
        .tick_interval_ms = 0,
    },    [G_KLOTSKI] = {
        .cat = CAT_LOGIC,
        .id = G_KLOTSKI, .title = "KLOTSKI", .tagline = "SLIDE THE BLOCK",
        .help = { "KLOTSKI", "ARROWS/WASD: SLIDE INTO GAP", "FREE THE 2X2 CAO BLOCK", "THROUGH THE BOTTOM EXIT", NULL },
        .enter = klotski_enter, .exit = klotski_exit,
        .tick = klotski_tick, .render = klotski_render, .on_key = klotski_on_key,
        .tick_interval_ms = 0,
    },
    [G_MAZE] = {
        .cat = CAT_LOGIC,
        .id = G_MAZE, .title = "MAZE", .tagline = "FIND THE WAY OUT",
        .help = { "MAZE", "ARROWS/WASD: MOVE", "REACH THE STAR", "S: SHOW SOLUTION", "N: NEW MAZE", NULL },
        .enter = maze_enter, .exit = maze_exit,
        .tick = maze_tick, .render = maze_render, .on_key = maze_on_key,
        .tick_interval_ms = 0,
    },
    [G_PEG] = {
        .cat = CAT_LOGIC,
        .id = G_PEG, .title = "PEG SOLITAIRE", .tagline = "JUMP TO ONE",
        .help = { "PEG SOLITAIRE", "OK: SELECT PEG", "JUMP OVER A PEG TO AN EMPTY", "LEAVE ONE PEG IN CENTER", NULL },
        .enter = peg_enter, .exit = peg_exit,
        .tick = peg_tick, .render = peg_render, .on_key = peg_on_key,
        .tick_interval_ms = 0,
    },
    [G_CONNECT4] = {
        .cat = CAT_BOARD,
        .id = G_CONNECT4, .title = "CONNECT 4", .tagline = "FOUR IN A ROW",
        .help = { "CONNECT 4", "LT/RT: SELECT COLUMN", "OK: DROP DISC", "FIRST TO FOUR IN A ROW", NULL },
        .enter = connect4_enter, .exit = connect4_exit,
        .tick = connect4_tick, .render = connect4_render, .on_key = connect4_on_key,
        .tick_interval_ms = 0,
    },
    [G_LIFE] = {
        .cat = CAT_ACTION,
        .id = G_LIFE, .title = "GAME OF LIFE", .tagline = "CELLULAR AUTOMATON",
        .help = { "LIFE", "OK: TOGGLE CELL  P: RUN/STOP", "S: STEP  R: RANDOM", "C: CLEAR  N: NEW", NULL },
        .enter = life_enter, .exit = life_exit,
        .tick = life_tick, .render = life_render, .on_key = life_on_key,
        .tick_interval_ms = 100,
    },
    [G_MASTERMIND] = {
        .cat = CAT_NUMBERS,
        .id = G_MASTERMIND, .title = "MASTERMIND", .tagline = "BREAK THE CODE",
        .help = { "MASTERMIND", "LT/RT: PICK POSITION", "UP/DN: CHANGE SYMBOL", "OK: SUBMIT ROW", NULL },
        .enter = mastermind_enter, .exit = mastermind_exit,
        .tick = mastermind_tick, .render = mastermind_render, .on_key = mastermind_on_key,
        .tick_interval_ms = 0,
    },
    [G_HANGMAN] = {
        .cat = CAT_WORDS,
        .id = G_HANGMAN, .title = "HANGMAN", .tagline = "SAVE THE MAN",
        .help = { "HANGMAN", "TYPE LETTERS TO GUESS", "6 WRONG AND HE HANGS", "N: NEW WORD", NULL },
        .enter = hangman_enter, .exit = hangman_exit,
        .tick = hangman_tick, .render = hangman_render, .on_key = hangman_on_key,
        .tick_interval_ms = 0,
    },
    [G_NONOGRAM] = {
        .cat = CAT_LOGIC,
        .id = G_NONOGRAM, .title = "NONOGRAM", .tagline = "PICTURE LOGIC",
        .help = { "NONOGRAM", "ARROWS/WASD: MOVE", "OK: PAINT  X: MARK", "PAINT TO MATCH THE HINTS", NULL },
        .enter = nonogram_enter, .exit = nonogram_exit,
        .tick = nonogram_tick, .render = nonogram_render, .on_key = nonogram_on_key,
        .tick_interval_ms = 0,
    },    [G_BULLSCOWS] = {
        .cat = CAT_NUMBERS,
        .id = G_BULLSCOWS, .title = "BULLS & COWS", .tagline = "GUESS THE 4 DIGITS",
        .help = { "BULLS & COWS", "LT/RT: POSITION  UP/DN: DIGIT", "OK: SUBMIT GUESS", "B=RIGHT DIGIT+SPOT C=RIGHT DIGIT", NULL },
        .enter = bullscows_enter, .exit = bullscows_exit,
        .tick = bullscows_tick, .render = bullscows_render, .on_key = bullscows_on_key,
        .tick_interval_ms = 0,
    },
    [G_DOTSBOX] = {
        .cat = CAT_BOARD,
        .id = G_DOTSBOX, .title = "DOTS & BOXES", .tagline = "CLAIM THE BOXES",
        .help = { "DOTS & BOXES", "ARROWS/WASD: MOVE  OK: DRAW", "COMPLETE A BOX TO CLAIM IT", "MORE BOXES WINS", NULL },
        .enter = dotsbox_enter, .exit = dotsbox_exit,
        .tick = dotsbox_tick, .render = dotsbox_render, .on_key = dotsbox_on_key,
        .tick_interval_ms = 0,
    },
    [G_BLACKJACK] = {
        .cat = CAT_CARDS,
        .id = G_BLACKJACK, .title = "BLACKJACK", .tagline = "BEAT THE DEALER",
        .help = { "BLACKJACK", "OK: HIT  BACK/SPACE: STAND", "GET 21, BEAT THE DEALER", "A COUNTS 1 OR 11", NULL },
        .enter = blackjack_enter, .exit = blackjack_exit,
        .tick = blackjack_tick, .render = blackjack_render, .on_key = blackjack_on_key,
        .tick_interval_ms = 800,
    },
    [G_PYRAMID] = {
        .cat = CAT_CARDS,
        .id = G_PYRAMID, .title = "PYRAMID", .tagline = "PAIR TO 13",
        .help = { "PYRAMID SOLITAIRE", "PICK 2 UNCOVERED CARDS", "SUM 13 TO REMOVE", "K REMOVES ALONE  BACK: DRAW", NULL },
        .enter = pyramid_enter, .exit = pyramid_exit,
        .tick = pyramid_tick, .render = pyramid_render, .on_key = pyramid_on_key,
        .tick_interval_ms = 0,
    },
    [G_GOLF] = {
        .cat = CAT_CARDS,
        .id = G_GOLF, .title = "GOLF SOLITAIRE", .tagline = "MATCH ADJACENT",
        .help = { "GOLF SOLITAIRE", "MOVE TOP CARD NEXT TO WASTE", "ADJACENT RANK OR A-K WRAP", "CLEAR ALL COLUMNS TO WIN", NULL },
        .enter = golf_enter, .exit = golf_exit,
        .tick = golf_tick, .render = golf_render, .on_key = golf_on_key,
        .tick_interval_ms = 0,
    },
    [G_KLONDIKE] = {
        .cat = CAT_CARDS,
        .id = G_KLONDIKE, .title = "KLONDIKE", .tagline = "CLASSIC SOLITAIRE",
        .help = { "KLONDIKE", "MOVE CARDS TO FOUNDATION", "RED/BLACK ALTERNATE ON TABLEAU", "K TO EMPTY  BACK: DRAW", NULL },
        .enter = klondike_enter, .exit = klondike_exit,
        .tick = klondike_tick, .render = klondike_render, .on_key = klondike_on_key,
        .tick_interval_ms = 0,
    },
    [G_FREECELL] = {
        .cat = CAT_CARDS,
        .id = G_FREECELL, .title = "FREECELL", .tagline = "ALL CARDS FACE UP",
        .help = { "FREECELL", "MOVE CARDS TO FOUNDATIONS", "FREECELLS HOLD ONE CARD", "ALL 52 TO THE TOP TO WIN", NULL },
        .enter = freecell_enter, .exit = freecell_exit,
        .tick = freecell_tick, .render = freecell_render, .on_key = freecell_on_key,
        .tick_interval_ms = 0,
    },
    [G_WHACK] = {
        .cat = CAT_ACTION,
        .id = G_WHACK, .title = "WHACK-A-MOLE", .tagline = "60 SECONDS",
        .help = { "WHACK-A-MOLE", "OK: WHACK THE MOLE", "60 SECONDS, MAX SCORE", "N: NEW GAME", NULL },
        .enter = whack_enter, .exit = whack_exit,
        .tick = whack_tick, .render = whack_render, .on_key = whack_on_key,
        .tick_interval_ms = 800,
    },    [G_KAKURO] = {
        .cat = CAT_LOGIC,
        .id = G_KAKURO, .title = "KAKURO", .tagline = "ARITHMETIC SUDOKU",
        .help = { "KAKURO", "FILL DIGITS SO EACH RUN", "SUMS TO ITS CLUE", "1-9 NO REPEAT IN A RUN", "N: NEXT  R: RESET", NULL },
        .enter = kakuro_enter, .exit = kakuro_exit,
        .tick = kakuro_tick, .render = kakuro_render, .on_key = kakuro_on_key,
        .tick_interval_ms = 0,
    },
    [G_NIM] = {
        .cat = CAT_BOARD,
        .id = G_NIM, .title = "NIM", .tagline = "TAKE THE LAST",
        .help = { "NIM", "LT/RT: RULE  OK: START", "UP/DN: PICK HEAP  OK: TAKE", "TAKE 1-3, LAST STONE WINS", NULL },
        .enter = nim_enter, .exit = nim_exit,
        .tick = nim_tick, .render = nim_render, .on_key = nim_on_key,
        .tick_interval_ms = 0,
    },
    [G_BATTLESHIP] = {
        .cat = CAT_DICE,
        .id = G_BATTLESHIP, .title = "BATTLESHIP", .tagline = "SINK THE FLEET",
        .help = { "BATTLESHIP", "PLACE 5 SHIPS THEN FIRE", "HIT: DOT  SINK: X", "SINK ALL ENEMY SHIPS", NULL },
        .enter = battleship_enter, .exit = battleship_exit,
        .tick = battleship_tick, .render = battleship_render, .on_key = battleship_on_key,
        .tick_interval_ms = 0,
    },
    [G_YAHTZEE] = {
        .cat = CAT_DICE,
        .id = G_YAHTZEE, .title = "YAHTZEE", .tagline = "5 DICE",
        .help = { "YAHTZEE", "OK: ROLL  LT/RT+OK: HOLD", "UP: ROLL  DN: SCORE", "FILL ALL 13 CATEGORIES", NULL },
        .enter = yahtzee_enter, .exit = yahtzee_exit,
        .tick = yahtzee_tick, .render = yahtzee_render, .on_key = yahtzee_on_key,
        .tick_interval_ms = 0,
    },
    [G_HEX] = {
        .cat = CAT_BOARD,
        .id = G_HEX, .title = "HEX", .tagline = "CONNECT THE SIDES",
        .help = { "HEX", "YOU: RED LEFT-RIGHT", "AI: BLUE TOP-BOTTOM", "CONNECT YOUR SIDES FIRST", NULL },
        .enter = hex_enter, .exit = hex_exit,
        .tick = hex_tick, .render = hex_render, .on_key = hex_on_key,
        .tick_interval_ms = 0,
    },
    [G_HEXMINES] = {
        .cat = CAT_LOGIC,
        .id = G_HEXMINES, .title = "HEX MINES", .tagline = "6-NEIGHBOR MINES",
        .help = { "HEX MINES", "ARROWS/WASD: MOVE  OK: REVEAL", "F/DEL: FLAG", "FIRST REVEAL IS SAFE", NULL },
        .enter = hexmines_enter, .exit = hexmines_exit,
        .tick = hexmines_tick, .render = hexmines_render, .on_key = hexmines_on_key,
        .tick_interval_ms = 0,
    },
    [G_CHECKERS] = {
        .cat = CAT_BOARD,
        .id = G_CHECKERS, .title = "CHECKERS", .tagline = "JUMP & CAPTURE",
        .help = { "CHECKERS", "OK: SELECT  OK: MOVE", "JUMP TO CAPTURE", "KING AT FAR ROW", NULL },
        .enter = checkers_enter, .exit = checkers_exit,
        .tick = checkers_tick, .render = checkers_render, .on_key = checkers_on_key,
        .tick_interval_ms = 0,
    },
    [G_ISOLA] = {
        .cat = CAT_BOARD,
        .id = G_ISOLA, .title = "ISOLA", .tagline = "ISOLATE YOUR FOE",
        .help = { "ISOLA", "OK: TOGGLE MOVE/BREAK", "MOVE YOUR PIECE OR BREAK A TILE", "STRANDED PLAYER LOSES", NULL },
        .enter = isola_enter, .exit = isola_exit,
        .tick = isola_tick, .render = isola_render, .on_key = isola_on_key,
        .tick_interval_ms = 0,
    },    [G_CHESS] = {
        .cat = CAT_BOARD,
        .id = G_CHESS, .title = "CHESS", .tagline = "THE ROYAL GAME",
        .help = { "CHESS", "OK: SELECT PIECE  OK: MOVE", "CAPTURE THE KING TO WIN", "SIMPLIFIED RULES", NULL },
        .enter = chess_enter, .exit = chess_exit,
        .tick = chess_tick, .render = chess_render, .on_key = chess_on_key,
        .tick_interval_ms = 0,
    },
    [G_DARKCHESS] = {
        .cat = CAT_BOARD,
        .id = G_DARKCHESS, .title = "DARK CHESS", .tagline = "FLIP & CAPTURE",
        .help = { "DARK CHESS", "OK: FLIP OR EAT ADJACENT", "RANK: J>S>X>M>C>P>B", "B PAWN BEATS J (CYCLE)", "EMPTY THE ENEMY", NULL },
        .enter = darkchess_enter, .exit = darkchess_exit,
        .tick = darkchess_tick, .render = darkchess_render, .on_key = darkchess_on_key,
        .tick_interval_ms = 100,
    },
    [G_BINARYPUZZLE] = {
        .cat = CAT_LOGIC,
        .id = G_BINARYPUZZLE, .title = "BINARY PUZZLE", .tagline = "0S AND 1S",
        .help = { "BINARY PUZZLE", "EQUAL 0/1 PER ROW/COL", "NO 3 IN A ROW", "NO DUPLICATE ROWS/COLS", "N: NEXT  R: RESET", NULL },
        .enter = binarypuzzle_enter, .exit = binarypuzzle_exit,
        .tick = binarypuzzle_tick, .render = binarypuzzle_render, .on_key = binarypuzzle_on_key,
        .tick_interval_ms = 0,
    },
    [G_FUTOSHIKI] = {
        .cat = CAT_LOGIC,
        .id = G_FUTOSHIKI, .title = "FUTOSHIKI", .tagline = "INEQUALITY SUDOKU",
        .help = { "FUTOSHIKI", "FILL 1-4, NO REPEAT", "RESPECT THE < > SIGNS", "N: NEXT  R: RESET", NULL },
        .enter = futoshiki_enter, .exit = futoshiki_exit,
        .tick = futoshiki_tick, .render = futoshiki_render, .on_key = futoshiki_on_key,
        .tick_interval_ms = 0,
    },
    [G_SKYSCRAPERS] = {
        .cat = CAT_LOGIC,
        .id = G_SKYSCRAPERS, .title = "SKYSCRAPERS", .tagline = "VISIBLE SKYLINE",
        .help = { "SKYSCRAPERS", "FILL 1-4, NO REPEAT", "EDGE NUMBERS = VISIBLE", "TALLER BLOCKS SHORTER", NULL },
        .enter = skyscrapers_enter, .exit = skyscrapers_exit,
        .tick = skyscrapers_tick, .render = skyscrapers_render, .on_key = skyscrapers_on_key,
        .tick_interval_ms = 0,
    },
    [G_NUMBERLINK] = {
        .cat = CAT_LOGIC,
        .id = G_NUMBERLINK, .title = "NUMBERLINK", .tagline = "CONNECT THE PAIRS",
        .help = { "NUMBERLINK", "OK: START LINE  MOVE: DRAW", "CONNECT SAME NUMBERS", "NO CROSSING PATHS", NULL },
        .enter = numberlink_enter, .exit = numberlink_exit,
        .tick = numberlink_tick, .render = numberlink_render, .on_key = numberlink_on_key,
        .tick_interval_ms = 0,
    },
    [G_ANAGRAMS] = {
        .cat = CAT_WORDS,
        .id = G_ANAGRAMS, .title = "ANAGRAMS", .tagline = "SCRAMBLED WORDS",
        .help = { "ANAGRAMS", "PICK LETTERS TO SPELL", "THE SCRAMBLED WORD", "DEL: UNDO  N: NEW", NULL },
        .enter = anagrams_enter, .exit = anagrams_exit,
        .tick = anagrams_tick, .render = anagrams_render, .on_key = anagrams_on_key,
        .tick_interval_ms = 0,
    },
    [G_QUESTIONS] = {
        .cat = CAT_WORDS,
        .id = G_QUESTIONS, .title = "20 QUESTIONS", .tagline = "THINK OF AN ANIMAL",
        .help = { "20 QUESTIONS", "THINK OF AN ANIMAL", "Y/N ANSWERS, I GUESS", "BEAT ME IF I GUESS WRONG", NULL },
        .enter = questions_enter, .exit = questions_exit,
        .tick = questions_tick, .render = questions_render, .on_key = questions_on_key,
        .tick_interval_ms = 0,
    },    [G_KILLERSUDOKU] = {
        .cat = CAT_LOGIC,
        .id = G_KILLERSUDOKU, .title = "KILLER SUDOKU", .tagline = "CAGES & SUMS",
        .help = { "KILLER SUDOKU", "SUDOKU + CAGE SUM CLUES", "EACH CAGE SUMS TO ITS CLUE", "P: CANDIDATES  N: NEXT", NULL },
        .enter = killersudoku_enter, .exit = killersudoku_exit,
        .tick = killersudoku_tick, .render = killersudoku_render, .on_key = killersudoku_on_key,
        .tick_interval_ms = 0,
    },
    [G_NURIKABE] = {
        .cat = CAT_LOGIC,
        .id = G_NURIKABE, .title = "NURIKABE", .tagline = "ISLANDS & WALLS",
        .help = { "NURIKABE", "OK: BLACK/WHITE  DEL: UNKNOWN", "ISLANDS MATCH THEIR NUMBERS", "WALL ONE PIECE, NO 2X2", NULL },
        .enter = nurikabe_enter, .exit = nurikabe_exit,
        .tick = nurikabe_tick, .render = nurikabe_render, .on_key = nurikabe_on_key,
        .tick_interval_ms = 0,
    },
    [G_PENTOMINO] = {
        .cat = CAT_LOGIC,
        .id = G_PENTOMINO, .title = "PENTOMINO", .tagline = "12 PIECES, ONE RECT",
        .help = { "PENTOMINO", "PLACE ALL 12 PIECES", "R: ROTATE  DEL: REMOVE", "FILL THE 6X10 BOARD", NULL },
        .enter = pentomino_enter, .exit = pentomino_exit,
        .tick = pentomino_tick, .render = pentomino_render, .on_key = pentomino_on_key,
        .tick_interval_ms = 0,
    },
    [G_MATCHSTICK] = {
        .cat = CAT_LOGIC,
        .id = G_MATCHSTICK, .title = "MATCHSTICK", .tagline = "MOVE ONE TO FIX",
        .help = { "MATCHSTICK", "PICK A MATCH, MOVE IT", "MAKE THE EQUATION TRUE", "ONE MOVE ONLY", NULL },
        .enter = matchstick_enter, .exit = matchstick_exit,
        .tick = matchstick_tick, .render = matchstick_render, .on_key = matchstick_on_key,
        .tick_interval_ms = 0,
    },
    [G_SIMON] = {
        .cat = CAT_ACTION,
        .id = G_SIMON, .title = "SIMON", .tagline = "REPEAT THE SEQUENCE",
        .help = { "SIMON", "WATCH THE SEQUENCE", "REPEAT IT ON THE PADS", "EACH ROUND GROWS", NULL },
        .enter = simon_enter, .exit = simon_exit,
        .tick = simon_tick, .render = simon_render, .on_key = simon_on_key,
        .tick_interval_ms = 100,
    },
    [G_SPIDER] = {
        .cat = CAT_CARDS,
        .id = G_SPIDER, .title = "SPIDER", .tagline = "CLEAR THE TABLEAU",
        .help = { "SPIDER SOLITAIRE", "STACK DESCENDING CARDS", "BACK: DEAL A ROW", "CLEAR ALL 10 COLUMNS", NULL },
        .enter = spider_enter, .exit = spider_exit,
        .tick = spider_tick, .render = spider_render, .on_key = spider_on_key,
        .tick_interval_ms = 0,
    },
    [G_POKERDRAW] = {
        .cat = CAT_CARDS,
        .id = G_POKERDRAW, .title = "POKER DRAW", .tagline = "5-CARD DUEL",
        .help = { "POKER DRAW", "KEEP UP TO 3 CARDS", "DISCARD & DRAW, COMPARE", "BEST HAND WINS", NULL },
        .enter = pokerdraw_enter, .exit = pokerdraw_exit,
        .tick = pokerdraw_tick, .render = pokerdraw_render, .on_key = pokerdraw_on_key,
        .tick_interval_ms = 0,
    },
    [G_TOWERDEFENSE] = {
        .cat = CAT_SIM,
        .id = G_TOWERDEFENSE, .title = "TOWER DEFENSE", .tagline = "DEFEND THE BASE",
        .help = { "TOWER DEFENSE", "OK: BUILD TOWER  P: PAUSE", "KILL ENEMIES FOR GOLD", "SURVIVE ALL 5 WAVES", NULL },
        .enter = towerdefense_enter, .exit = towerdefense_exit,
        .tick = towerdefense_tick, .render = towerdefense_render, .on_key = towerdefense_on_key,
        .tick_interval_ms = 500,
    },    [G_CCHECKERS] = {
        .cat = CAT_BOARD,
        .id = G_CCHECKERS, .title = "CHINESE CHECKERS", .tagline = "JUMP ACROSS",
        .help = { "CHINESE CHECKERS", "OK: SELECT  OK: JUMP/MOVE", "JUMP OVER PIECES", "GET ALL 10 HOME", NULL },
        .enter = ccheckers_enter, .exit = ccheckers_exit,
        .tick = ccheckers_tick, .render = ccheckers_render, .on_key = ccheckers_on_key,
        .tick_interval_ms = 0,
    },
    [G_BACKGAMMON] = {
        .cat = CAT_BOARD,
        .id = G_BACKGAMMON, .title = "BACKGAMMON", .tagline = "DICE RACE",
        .help = { "BACKGAMMON", "OK: ROLL DICE", "PICK PIECE, ADVANCE", "FIRST ALL HOME WINS", NULL },
        .enter = backgammon_enter, .exit = backgammon_exit,
        .tick = backgammon_tick, .render = backgammon_render, .on_key = backgammon_on_key,
        .tick_interval_ms = 100,
    },
    [G_BOGGLE] = {
        .cat = CAT_WORDS,
        .id = G_BOGGLE, .title = "BOGGLE", .tagline = "WORD HUNT",
        .help = { "BOGGLE", "OK: PICK LETTER  BACK: UNDO", "SPELL WORDS 3+ LETTERS", "ADJACENT CELLS ONLY", "60 SECONDS", NULL },
        .enter = boggle_enter, .exit = boggle_exit,
        .tick = boggle_tick, .render = boggle_render, .on_key = boggle_on_key,
        .tick_interval_ms = 100,
    },
    [G_WORDCHAIN] = {
        .cat = CAT_WORDS,
        .id = G_WORDCHAIN, .title = "WORD CHAIN", .tagline = "LINK THE WORDS",
        .help = { "WORD CHAIN", "TYPE A WORD, FIRST LETTER", "MATCHES LAST LETTER", "RUN OUT OF WORDS = LOSE", NULL },
        .enter = wordchain_enter, .exit = wordchain_exit,
        .tick = wordchain_tick, .render = wordchain_render, .on_key = wordchain_on_key,
        .tick_interval_ms = 0,
    },
    [G_MATHTRAIN] = {
        .cat = CAT_NUMBERS,
        .id = G_MATHTRAIN, .title = "MATH TRAINER", .tagline = "QUICK ARITHMETIC",
        .help = { "MATH TRAINER", "TYPE THE ANSWER", "OK: SUBMIT  10 CORRECT = LV UP", "60 SECONDS", NULL },
        .enter = mathtrain_enter, .exit = mathtrain_exit,
        .tick = mathtrain_tick, .render = mathtrain_render, .on_key = mathtrain_on_key,
        .tick_interval_ms = 100,
    },
    [G_PRIMERUSH] = {
        .cat = CAT_NUMBERS,
        .id = G_PRIMERUSH, .title = "PRIME RUSH", .tagline = "PRIME OR NOT?",
        .help = { "PRIME RUSH", "YES = OK  NO = BACK", "ANSWER FAST, EARN POINTS", "60 SECONDS", NULL },
        .enter = primerush_enter, .exit = primerush_exit,
        .tick = primerush_tick, .render = primerush_render, .on_key = primerush_on_key,
        .tick_interval_ms = 100,
    },
    [G_BINMORSE] = {
        .cat = CAT_NUMBERS,
        .id = G_BINMORSE, .title = "BINARY & MORSE", .tagline = "GEEK DRILL",
        .help = { "BINARY & MORSE", "DEC<->BIN: TYPE 0/1  OK: SUBMIT", "MORSE: LEFT= DOT  RIGHT= DASH", "60 SECONDS", NULL },
        .enter = binmorse_enter, .exit = binmorse_exit,
        .tick = binmorse_tick, .render = binmorse_render, .on_key = binmorse_on_key,
        .tick_interval_ms = 100,
    },
    [G_TICTACDICE] = {
        .cat = CAT_DICE,
        .id = G_TICTACDICE, .title = "TIC-TAC-DICE", .tagline = "ROLL & PLACE",
        .help = { "TIC-TAC-DICE", "OK: ROLL DIE", "PLACE IN THE ROLLED ROW", "3 IN A ROW WINS", NULL },
        .enter = tictacdice_enter, .exit = tictacdice_exit,
        .tick = tictacdice_tick, .render = tictacdice_render, .on_key = tictacdice_on_key,
        .tick_interval_ms = 0,
    },    [G_SLITHERLINK] = {
        .cat = CAT_LOGIC,
        .id = G_SLITHERLINK, .title = "SLITHERLINK", .tagline = "ONE LOOP",
        .help = { "SLITHERLINK", "DRAW ONE CLOSED LOOP", "EDGES MATCH THE NUMBERS", "OK: EMPTY/LINE/DOT", NULL },
        .enter = slitherlink_enter, .exit = slitherlink_exit,
        .tick = slitherlink_tick, .render = slitherlink_render, .on_key = slitherlink_on_key,
        .tick_interval_ms = 0,
    },
    [G_HASHI] = {
        .cat = CAT_LOGIC,
        .id = G_HASHI, .title = "HASHI", .tagline = "BRIDGE THE ISLANDS",
        .help = { "HASHI", "OK: PICK ISLAND, BRIDGE", "BRIDGE COUNT = NUMBER", "ALL CONNECTED", NULL },
        .enter = hashi_enter, .exit = hashi_exit,
        .tick = hashi_tick, .render = hashi_render, .on_key = hashi_on_key,
        .tick_interval_ms = 0,
    },
    [G_STARBATTLE] = {
        .cat = CAT_LOGIC,
        .id = G_STARBATTLE, .title = "STAR BATTLE", .tagline = "2 STARS EVERYWHERE",
        .help = { "STAR BATTLE", "2 STARS PER ROW/COL/REGION", "STARS NEVER TOUCH", "OK: PLACE/REMOVE", NULL },
        .enter = starbattle_enter, .exit = starbattle_exit,
        .tick = starbattle_tick, .render = starbattle_render, .on_key = starbattle_on_key,
        .tick_interval_ms = 0,
    },
    [G_TENTS] = {
        .cat = CAT_LOGIC,
        .id = G_TENTS, .title = "TENTS", .tagline = "PITCH BY THE TREES",
        .help = { "TENTS", "EACH TREE ONE NEARBY TENT", "TENTS NEVER TOUCH", "ROW/COL TENT COUNTS", NULL },
        .enter = tents_enter, .exit = tents_exit,
        .tick = tents_tick, .render = tents_render, .on_key = tents_on_key,
        .tick_interval_ms = 0,
    },
    [G_KENKEN] = {
        .cat = CAT_LOGIC,
        .id = G_KENKEN, .title = "KENKEN", .tagline = "ARITHMETIC SUDOKU",
        .help = { "KENKEN", "1-4 PER ROW/COL", "CAGE MATH = CLUE", "N: NEXT  R: RESET", NULL },
        .enter = kenken_enter, .exit = kenken_exit,
        .tick = kenken_tick, .render = kenken_render, .on_key = kenken_on_key,
        .tick_interval_ms = 0,
    },
    [G_JIGSAW] = {
        .cat = CAT_LOGIC,
        .id = G_JIGSAW, .title = "JIGSAW", .tagline = "SLIDE & SORT",
        .help = { "JIGSAW", "SLIDE TILES INTO ORDER", "OK: SWAP WITH BLANK", "1..20 ROW-MAJOR", NULL },
        .enter = jigsaw_enter, .exit = jigsaw_exit,
        .tick = jigsaw_tick, .render = jigsaw_render, .on_key = jigsaw_on_key,
        .tick_interval_ms = 0,
    },
    [G_LIFE2] = {
        .cat = CAT_ACTION,
        .id = G_LIFE2, .title = "LIFE VARIANTS", .tagline = "CELLULAR EVOLUTION",
        .help = { "LIFE VARIANTS", "OK: RUN/PAUSE  R: RULE", "N: RANDOM  C: CLEAR", "EDIT WITH CURSOR", NULL },
        .enter = life2_enter, .exit = life2_exit,
        .tick = life2_tick, .render = life2_render, .on_key = life2_on_key,
        .tick_interval_ms = 400,
    },
    [G_MEMDIGITS] = {
        .cat = CAT_NUMBERS,
        .id = G_MEMDIGITS, .title = "MEMORY DIGITS", .tagline = "REPEAT THE NUMBER",
        .help = { "MEMORY DIGITS", "WATCH THE NUMBER", "TYPE IT BACK", "3 RIGHT = LV UP", NULL },
        .enter = memdigits_enter, .exit = memdigits_exit,
        .tick = memdigits_tick, .render = memdigits_render, .on_key = memdigits_on_key,
        .tick_interval_ms = 100,
    },    [G_GEO] = {
        .cat = CAT_QUIZ,
        .id = G_GEO, .title = "GEO QUIZ", .tagline = "COUNTRIES & CAPITALS",
        .help = { "GEO QUIZ", "TYPE THE CAPITAL", "OK: SUBMIT  DEL: ERASE", "N: FLIP DIRECTION", "30 SECONDS", NULL },
        .enter = geo_enter, .exit = geo_exit,
        .tick = geo_tick, .render = geo_render, .on_key = geo_on_key,
        .tick_interval_ms = 100,
    },
    [G_FLAGQUIZ] = {
        .cat = CAT_QUIZ,
        .id = G_FLAGQUIZ, .title = "FLAG QUIZ", .tagline = "NAME THE FLAG",
        .help = { "FLAG QUIZ", "PICK THE COUNTRY", "ARROWS: CHOOSE  OK: CONFIRM", "N: SKIP", NULL },
        .enter = flagquiz_enter, .exit = flagquiz_exit,
        .tick = flagquiz_tick, .render = flagquiz_render, .on_key = flagquiz_on_key,
        .tick_interval_ms = 0,
    },
    [G_ELEMENTS] = {
        .cat = CAT_QUIZ,
        .id = G_ELEMENTS, .title = "ELEMENT QUIZ", .tagline = "PERIODIC TABLE",
        .help = { "ELEMENT QUIZ", "GUESS THE ELEMENT", "ARROWS: CHOOSE  OK: CONFIRM", "OR TYPE IT OUT", "30 SECONDS", NULL },
        .enter = elements_enter, .exit = elements_exit,
        .tick = elements_tick, .render = elements_render, .on_key = elements_on_key,
        .tick_interval_ms = 100,
    },
    [G_TIMESTRAIN] = {
        .cat = CAT_NUMBERS,
        .id = G_TIMESTRAIN, .title = "TIMES TRAINER", .tagline = "MULTIPLY FAST",
        .help = { "TIMES TRAINER", "TYPE THE PRODUCT", "OK: SUBMIT", "COMBO BONUSES", "60 SECONDS", NULL },
        .enter = timestrain_enter, .exit = timestrain_exit,
        .tick = timestrain_tick, .render = timestrain_render, .on_key = timestrain_on_key,
        .tick_interval_ms = 100,
    },
    [G_CLOCKQUIZ] = {
        .cat = CAT_NUMBERS,
        .id = G_CLOCKQUIZ, .title = "CLOCK QUIZ", .tagline = "READ THE CLOCK",
        .help = { "CLOCK QUIZ", "READ THE HANDS", "TYPE HHMM (24H)", "OK: SUBMIT", "30 SECONDS", NULL },
        .enter = clockquiz_enter, .exit = clockquiz_exit,
        .tick = clockquiz_tick, .render = clockquiz_render, .on_key = clockquiz_on_key,
        .tick_interval_ms = 100,
    },
    [G_RIDDLE] = {
        .cat = CAT_WORDS,
        .id = G_RIDDLE, .title = "RIDDLE QUIZ", .tagline = "CLASSIC RIDDLES",
        .help = { "RIDDLE QUIZ", "ANSWER THE RIDDLE", "TYPE A-Z  OK: CHECK", "DEL: ERASE  N: SKIP", "BACK: PAUSE  Q: SUMMARY", NULL },
        .enter = riddle_enter, .exit = riddle_exit,
        .tick = riddle_tick, .render = riddle_render, .on_key = riddle_on_key,
        .tick_interval_ms = 700,
    },
    [G_DICEQUEST] = {
        .cat = CAT_DICE,
        .id = G_DICEQUEST, .title = "DICE QUEST", .tagline = "ROLL & EXPLORE",
        .help = { "DICE QUEST", "MOVE TO THE GEM", "? TILES: ROLL EVENTS", "KEY OPENS THE DOOR", NULL },
        .enter = dicequest_enter, .exit = dicequest_exit,
        .tick = dicequest_tick, .render = dicequest_render, .on_key = dicequest_on_key,
        .tick_interval_ms = 0,
    },
    [G_LIARSDICE] = {
        .cat = CAT_DICE,
        .id = G_LIARSDICE, .title = "LIAR'S DICE", .tagline = "BLUFF OR CALL",
        .help = { "LIAR'S DICE", "BID: # + VALUE", "SPACE/BACK: CALL BLUFF", "OUT OF DICE LOSES", NULL },
        .enter = liarsdice_enter, .exit = liarsdice_exit,
        .tick = liarsdice_tick, .render = liarsdice_render, .on_key = liarsdice_on_key,
        .tick_interval_ms = 0,
    },    [G_MINER] = {
        .cat = CAT_SIM,
        .id = G_MINER, .title = "MINER", .tagline = "DIG & UPGRADE",
        .help = { "MINER", "OK: DIG  SPACE: SHOP", "MINE ORE FOR GOLD", "UPGRADE TO DIG DEEPER", NULL },
        .enter = miner_enter, .exit = miner_exit,
        .tick = miner_tick, .render = miner_render, .on_key = miner_on_key,
        .tick_interval_ms = 0,
    },
    [G_RAILS] = {
        .cat = CAT_SIM,
        .id = G_RAILS, .title = "RAIL PLANNER", .tagline = "LINK THE STATIONS",
        .help = { "RAIL PLANNER", "OK: LAY RAIL  DEL: REMOVE", "CONNECT S TO ALL T", "TRACKS: STRAIGHT/CURVE/X", NULL },
        .enter = rails_enter, .exit = rails_exit,
        .tick = rails_tick, .render = rails_render, .on_key = rails_on_key,
        .tick_interval_ms = 0,
    },
    [G_CITYBUILDER] = {
        .cat = CAT_SIM,
        .id = G_CITYBUILDER, .title = "CITY BUILDER", .tagline = "ZONE THE GRID",
        .help = { "CITY BUILDER", "PLACE: H S P F  SPACE: CYCLE", "DEL: DEMOLISH", "MAXIMIZE NEIGHBOR SCORE", "20 TURNS", NULL },
        .enter = citybuilder_enter, .exit = citybuilder_exit,
        .tick = citybuilder_tick, .render = citybuilder_render, .on_key = citybuilder_on_key,
        .tick_interval_ms = 0,
    },
    [G_BEEFARM] = {
        .cat = CAT_SIM,
        .id = G_BEEFARM, .title = "BEE FARM", .tagline = "PLANT & HARVEST",
        .help = { "BEE FARM", "OK: PLANT/HARVEST  B: HIVE", "MORE FLOWERS = MORE HONEY", "30 HONEY IN 25 TURNS", NULL },
        .enter = beefarm_enter, .exit = beefarm_exit,
        .tick = beefarm_tick, .render = beefarm_render, .on_key = beefarm_on_key,
        .tick_interval_ms = 1000,
    },
    [G_MUSHGARDEN] = {
        .cat = CAT_SIM,
        .id = G_MUSHGARDEN, .title = "MUSHROOM GARDEN", .tagline = "GROW & HARVEST",
        .help = { "MUSHROOM GARDEN", "OK: PLANT/HARVEST/WAIT", "READY = HARVEST +2", "OVERDUE = ROT -1", "25 TURNS", NULL },
        .enter = mushgarden_enter, .exit = mushgarden_exit,
        .tick = mushgarden_tick, .render = mushgarden_render, .on_key = mushgarden_on_key,
        .tick_interval_ms = 1000,
    },
    [G_QUAKE] = {
        .cat = CAT_SIM,
        .id = G_QUAKE, .title = "QUAKE ESCAPE", .tagline = "DODGE THE FALLS",
        .help = { "QUAKE ESCAPE", "REACH THE EXIT", "CRACKS & FALLING ROCKS", "GET HIT = LOSE HP", NULL },
        .enter = quake_enter, .exit = quake_exit,
        .tick = quake_tick, .render = quake_render, .on_key = quake_on_key,
        .tick_interval_ms = 0,
    },
    [G_ANTCOLONY] = {
        .cat = CAT_SIM,
        .id = G_ANTCOLONY, .title = "ANT COLONY", .tagline = "FOOD ROUTES",
        .help = { "ANT COLONY", "STEER THE ANTS", "REACH FOOD, BRING IT BACK", "15 FOOD IN 25 TURNS", NULL },
        .enter = antcolony_enter, .exit = antcolony_exit,
        .tick = antcolony_tick, .render = antcolony_render, .on_key = antcolony_on_key,
        .tick_interval_ms = 1000,
    },
    [G_MAHJONGMATCH] = {
        .cat = CAT_DICE,
        .id = G_MAHJONGMATCH, .title = "MAHJONG MATCH", .tagline = "PAIR THE TILES",
        .help = { "MAHJONG MATCH", "PICK TWO MATCHING TILES", "NO COVER, ONE SIDE OPEN", "CLEAR THE BOARD", NULL },
        .enter = mahjongmatch_enter, .exit = mahjongmatch_exit,
        .tick = mahjongmatch_tick, .render = mahjongmatch_render, .on_key = mahjongmatch_on_key,
        .tick_interval_ms = 0,
    },









    [G_GOMOKU] = {
        .cat = CAT_BOARD,
        .id = G_GOMOKU, .title = "GOMOKU", .tagline = "FIVE IN A ROW",
        .help = { "GOMOKU", "ARROWS/WASD: MOVE  OK: PLACE", "YOU: BLACK  AI: WHITE", "FIRST TO FIVE IN A ROW WINS", NULL },
        .enter = gomoku_enter, .exit = gomoku_exit,
        .tick = gomoku_tick, .render = gomoku_render, .on_key = gomoku_on_key,
        .tick_interval_ms = 0,
    },
    [G_HANOI] = {
        .cat = CAT_BOARD,
        .id = G_HANOI, .title = "HANOI", .tagline = "TOWERS",
        .help = { "HANOI TOWERS", "GOAL: ALL DISKS TO STAR PEG", "ONE DISK AT A TIME", "NEVER BIG ON SMALL", "RANDOM LEVEL EACH GAME", NULL },
        .enter = hanoi_enter, .exit = hanoi_exit,
        .tick = hanoi_tick, .render = hanoi_render, .on_key = hanoi_on_key,
        .tick_interval_ms = 0,
    },
};
