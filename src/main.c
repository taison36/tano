#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8
#define INITIAL_LINES_CAPACITY 50

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) (a) > (b) ? (a) : (b)

struct termios orig_termios;
struct WindowSettings;
struct ScreenSettings;
struct TextBuffer;
struct VisualCache;
struct ScreenBuffer;

void bufferLoadCurLine(struct TextBuffer *buffer);
void vcache_schift_add_line(struct VisualCache *visual_cache,
                            struct WindowSettings *ws, int cur_y, char *line);
void curLineClearAndResetX(struct TextBuffer *buffer);
void curLineWriteChar(struct TextBuffer *buffer, char c);
void bufferSaveCurrentLine(struct TextBuffer *buffer);
void editorUpdateCursorCoordinates(struct TextBuffer *buffer,
                                   struct WindowSettings *ws,
                                   struct ScreenSettings *screen_settings,
                                   struct VisualCache *visual_cache);
void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws,
                         struct ScreenSettings *screen_settings);
void editorRefreshCursor(struct ScreenSettings *screen_settings);
void freeTextBuffer(struct TextBuffer *buffer);
void moveCursorDown(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings,
                    struct VisualCache *visual_cache,
                    struct WindowSettings *ws);
void die(const char *s);
void cleanEditor();
void curLineWriteChars(struct TextBuffer *buffer, const char *chars);
void bufferHandleNewLineInput(struct TextBuffer *buffer,
                              struct ScreenSettings *screen_settings,
                              struct VisualCache *visual_cache,
                              struct WindowSettings *ws);
int countNewLineChars(const char *str);
void editorEnsureLineCapacity(struct TextBuffer *buffer, int required_idx);
char *addNewLineChar(char *str);
void vcache_write_line(struct VisualCache *visual_cache,
                       struct WindowSettings *ws, int cur_y, char *line);
void calculate_screenY_and_first_printline(
    struct TextBuffer *buffer, struct ScreenSettings *screen_settings,
    struct WindowSettings *ws, struct VisualCache *visual_cache);
int getScreenLinesForString(const char *str, int screen_width);

// INIT
struct TextBuffer {
  char **lines;
  int lines_num;
  int lines_capacity;
  int cur_x;
  int cur_y;
  char cur_line[SIZELINE];
};

struct WindowSettings {
  int top_offset;
  int bottom_offset;
  int left_offset;
  int terminal_height;
  int terminal_width;
  int screen_width;
  int screen_height;
};

struct ScreenSettings {
  int cursor_x;
  int cursor_y;
  int logical_wanted_x;
  int first_printline;
};

struct VisualCache {
  int *lines_screen_height;
  int lines_num;
  int lines_capacity;
  int prefix_sum_line_heights[2500];
};

struct ScreenBuffer {
  char *content;
  int appended;
  int size;
  int rows_num;
};

typedef enum {
  PANEL_DEFAULT,
  PANEL_QUIT_CONFIRM,
  PANEL_HELP,
  PANEL_COUNT
} BottomPanelMessage;

static struct TextBuffer *global_buffer_for_cleanup;
static int global_buffer_initialized = 0;

static const char *panel_bottom_messages[PANEL_COUNT] = {
    [PANEL_DEFAULT] = "\x1b[30;47m ^Q Exit  ^H Help \x1b[0m",
    [PANEL_QUIT_CONFIRM] = "\x1b[30;47m Do you want to save the changes, "
                           "buddy? [Y]es / [N]o \x1b[0m",
    [PANEL_HELP] = "\x1b[30;47m Nobody can help you, man \x1b[0m"};
static BottomPanelMessage panel_current_message = PANEL_DEFAULT;
static const char *input_file_path;

struct ScreenBuffer screen_buffer_init() {
  struct ScreenBuffer screen_buffer;
  screen_buffer.size = SIZELINE;
  screen_buffer.appended = 0;
  screen_buffer.content = malloc(screen_buffer.size);
  screen_buffer.rows_num = 0;
  if (!screen_buffer.content)
    die("editorPrepareBufferForScreen: malloc failed");
  return screen_buffer;
}

struct TextBuffer textBufferInit() {
  struct TextBuffer buffer;
  buffer.cur_x = 0;
  buffer.cur_y = 0;
  buffer.lines_num = 0;
  buffer.lines_capacity = INITIAL_LINES_CAPACITY;
  buffer.lines = malloc(buffer.lines_capacity * sizeof(char *));
  if (buffer.lines == NULL) {
    die("textBufferInit: malloc for lines failed");
  }

  for (int i = 0; i < buffer.lines_capacity; i++) {
    buffer.lines[i] = NULL;
  }

  buffer.cur_line[0] = '\0';

  // For atexit cleanup
  global_buffer_for_cleanup = &buffer;
  global_buffer_initialized = 1;
  return buffer;
}

struct VisualCache visualCacheInit() {
  struct VisualCache visual_cache;
  visual_cache.lines_num = 0;
  visual_cache.lines_capacity = INITIAL_LINES_CAPACITY;
  visual_cache.lines_screen_height =
      malloc(visual_cache.lines_capacity * sizeof(int));

  if (visual_cache.lines_screen_height == NULL) {
    die("visualCacheInit: malloc failed");
  }

  return visual_cache;
}

struct WindowSettings windowSettingsInit() {
  struct WindowSettings ws;

  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
    die("windowSettingsInit: ioctl. Cannot proceed without terminal size");
  }

  ws.top_offset = 0;
  ws.bottom_offset =
      getScreenLinesForString(panel_bottom_messages[PANEL_DEFAULT],
                              w.ws_col); // +1 for the space beetwen text
  ws.left_offset = 0;

  ws.terminal_width = w.ws_col;
  ws.terminal_height = w.ws_row;

  // ws.terminal_width = 40;
  // ws.terminal_height = 20;

  ws.screen_width = ws.terminal_width - ws.left_offset;
  ws.screen_height = ws.terminal_height - (ws.bottom_offset + ws.top_offset);

  return ws;
}

// TERMINAL
void die(const char *s) {
  cleanEditor();
  perror(s);
  exit(1);
}

void switchToMainScreen() {
  write(STDOUT_FILENO, "\x1b[?1049l", 8);
  write(STDOUT_FILENO, "\x1b[?1000l", 8);
  write(STDOUT_FILENO, "\x1b[?1006l", 8);
}

void switchToAlternateScreen() {
  atexit(switchToMainScreen);

  write(STDOUT_FILENO, "\x1b[?1049h", 8);

  write(STDOUT_FILENO, "\x1b[?1000h", 8);
  write(STDOUT_FILENO, "\x1b[?1006h", 8);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcssetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);

  raw.c_cflag |= (CS8);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

// HELPER
char *addNewLineChar(char *str) {
  if (str == NULL) {
    return NULL;
  }
  const size_t str_len = strlen(str);

  char *temp = realloc(str, str_len + 3); // +2 for \r\n, +1 for \0
  if (temp == NULL) {
    die("addNewLineChar: realloc failed");
  }

  str = temp;

  str[str_len] = '\r';
  str[str_len + 1] = '\n';
  str[str_len + 2] = '\0';
  return str;
}

char **splitLine(const char *str, size_t n) {
  if (str == NULL) {
    return NULL;
  }

  size_t str_len = strlen(str);
  if (n > str_len) {
    return NULL;
  }

  char **res = malloc(2 * sizeof(*res));
  if (res == NULL) {
    die("splitLine: failed malloc");
  }

  size_t second_part_len = str_len - n;

  res[0] = malloc(n + 1);
  res[1] = malloc(second_part_len + 1);

  if (res[0] == NULL || res[1] == NULL) {
    free(res[0]);
    free(res[1]);
    free(res);
    die("splitLine: failed to allocate memory for a string");
  }

  memcpy(res[0], str, n);
  res[0][n] = '\0';

  memcpy(res[1], str + n, second_part_len);
  res[1][second_part_len] = '\0';

  return res;
}

int countNewLineChars(const char *str) {
  if (str == NULL) {
    return 0;
  }

  size_t len = strlen(str);

  if (len >= 2 && str[len - 2] == '\r' && str[len - 1] == '\n') {
    return 2;
  } else if (len >= 1 && str[len - 1] == '\n') {
    return 1;
  } else if (len >= 1 && str[len - 1] == '\r') {
    return 1;
  }

  return 0;
}

void cleanEditor() {
  if (global_buffer_initialized) {
    freeTextBuffer(global_buffer_for_cleanup);
    global_buffer_initialized = 0;
  }
}

void freeTextBuffer(struct TextBuffer *buffer) {
  if (buffer == NULL || buffer->lines == NULL)
    return;
  for (int i = 0; i < buffer->lines_num; i++) {
    if (buffer->lines[i] != NULL) {
      free(buffer->lines[i]);
      buffer->lines[i] = NULL;
    }
  }
  for (int i = buffer->lines_num; i < buffer->lines_capacity; i++) {
    if (buffer->lines[i] != NULL) {
      free(buffer->lines[i]);
      buffer->lines[i] = NULL;
    }
  }
  free(buffer->lines);
  buffer->lines = NULL;
  buffer->lines_num = 0;
  buffer->lines_capacity = 0;
}

void copyLine(char *from, char *to) {
  if (from == NULL) {
    *to = '\0';
    return;
  }

  while (*from != '\0') {
    *to++ = *from++;
  }
  *to = '\0';
}

char *appendTwoLines(const char *str1, const char *str2) {
  if (str1 == NULL)
    str1 = "";
  if (str2 == NULL)
    str2 = "";

  size_t len_str1 = strlen(str1);
  size_t len_str2 = strlen(str2);
  size_t total_len =
      len_str1 + len_str2 + 1; // +1 for the final null terminator

  char *new_line = malloc(total_len);

  if (new_line == NULL) {
    die("appendTwoLines: malloc failed");
  }

  memcpy(new_line, str1, len_str1);

  memcpy(new_line + len_str1, str2, len_str2);

  new_line[total_len - 1] = '\0';

  return new_line;
}

int isInputAvailable() {
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  int ret = poll(&pfd, 1, 0);
  return ret > 0;
}

void moveCharsRight(char *str, int cur_pos, int n) {
  int len = strlen(str);

  for (int i = len; i >= cur_pos; i--) {
    str[i + n] = str[i];
  }
}

void moveCharsLeft(char *str, int cur_pos, int n) {
  int len = strlen(str);

  if (cur_pos < n)
    return;

  for (int i = cur_pos; i <= len; i++) {
    str[i - n] = str[i];
  }
}

void moveRowsUp(char **lines, int *num_lines, int row_to_delete) {
  // so cur_row disappears
  if (lines == NULL || row_to_delete < 0)
    return;

  if (lines[row_to_delete] != NULL) {
    free(lines[row_to_delete]);
    lines[row_to_delete] = NULL;
  }

  for (int i = row_to_delete; i < *num_lines; i++) {
    lines[i] = lines[i + 1];
  }

  (*num_lines)--;
}

void moveIntsUp(int *arr, int *num_elements, int index_to_delete) {
  if (arr == NULL || index_to_delete < 0 || index_to_delete >= *num_elements) {
    return;
  }

  for (int i = index_to_delete; i < *num_elements - 1; i++) {
    arr[i] = arr[i + 1];
  }

  (*num_elements)--;
}

void moveIntsDown(int *arr, int index_to_insert, int *num_elements) {
  if (arr == NULL || index_to_insert < 0 || index_to_insert > *num_elements)
    return;

  for (int i = *num_elements; i > index_to_insert; i--) {
    arr[i] = arr[i - 1];
  }

  arr[index_to_insert] = -1;
  (*num_elements)++;
}

void moveRowsDown(char **lines, int row_to_move, int *num_lines) {
  if (lines == NULL || row_to_move < 0)
    return;

  for (int i = *num_lines; i > row_to_move; i--) {
    lines[i] = lines[i - 1];
  }

  lines[row_to_move] = NULL;

  (*num_lines)++;
}

char *makeStringFromInt(int n) {
  char *str = malloc(12);
  if (!str)
    die("makeStringFromInt: malloc failed");
  snprintf(str, 12, "%d", n);
  return str;
}

int getScreenLinesForString(const char *str, int screen_width) {
  if (str == NULL) {
    return 1;
  }
  int len = strlen(str);
  int stringLength = len;

  stringLength -= countNewLineChars(str);

  if (stringLength == 0) {
    return 1;
  }

  if (screen_width == 0) {
    return 1;
  }
  return (stringLength / screen_width) +
         ((stringLength % screen_width != 0) ? 1 : 0);
}

// DYNAMIC ARRAY MANAGEMENT for buffer->lines
void editorEnsureLineCapacity(struct TextBuffer *buffer, int lines_num) {
  if (lines_num >= buffer->lines_capacity) {
    int newCapacity = buffer->lines_capacity == 0 ? INITIAL_LINES_CAPACITY
                                                  : buffer->lines_capacity * 2;
    while (newCapacity <= lines_num) {
      newCapacity *= 2;
    }
    char **new_lines = realloc(buffer->lines, newCapacity * sizeof(char *));
    if (!new_lines)
      die("editorEnsureLineCapacity: realloc lines failed");

    for (int i = buffer->lines_capacity; i < newCapacity; i++) {
      new_lines[i] = NULL;
    }
    buffer->lines = new_lines;
    buffer->lines_capacity = newCapacity;
  }
}

// CURSOR
void editorUpdateCursorCoordinates(struct TextBuffer *buffer,
                                   struct WindowSettings *ws,
                                   struct ScreenSettings *screen_settings,
                                   struct VisualCache *visual_cache) {
  calculate_screenY_and_first_printline(buffer, screen_settings, ws,
                                        visual_cache);

  int y = 0;
  for (int i = screen_settings->first_printline; i < buffer->cur_y; i++) {
    y += visual_cache->lines_screen_height[i];
  }
  y += (ws->screen_width > 0) ? (buffer->cur_x / ws->screen_width) + 1 : 0;

  screen_settings->cursor_y = y;
  screen_settings->cursor_x = (ws->screen_width > 0)
                                  ? (buffer->cur_x % ws->screen_width) + 1
                                  : buffer->cur_x + 1;
}

void editorRefreshCursor(struct ScreenSettings *screen_settings) {
  char *x = makeStringFromInt(screen_settings->cursor_x);
  char *y = makeStringFromInt(screen_settings->cursor_y);
  int len = 5 + strlen(x) + strlen(y);
  char *str = malloc(len + 1);

  if (!str) {
    free(x);
    free(y);
    die("editorRefreshScreen: malloc failed");
  }

  strcpy(str, "\x1b[");
  strcat(str, y);
  strcat(str, ";");
  strcat(str, x);
  strcat(str, "H");

  write(STDOUT_FILENO, str, len);

  free(y);
  free(x);
  free(str);
}

void moveCursorRight(struct TextBuffer *buffer,
                     struct ScreenSettings *screen_settings,
                     struct VisualCache *visual_cache,
                     struct WindowSettings *ws) {
  int stringLength = strlen(buffer->cur_line);
  int count_newline = countNewLineChars(buffer->cur_line);
  if (count_newline == 2) {
    stringLength -= 2;
  } else if (count_newline == 1) {
    stringLength -= 1;
  }

  if (buffer->cur_x < stringLength) {
    buffer->cur_x++;
    screen_settings->logical_wanted_x = buffer->cur_x;
  } else if (buffer->cur_y < buffer->lines_num) {
    bufferSaveCurrentLine(buffer);
    screen_settings->logical_wanted_x = 1;
    moveCursorDown(buffer, screen_settings, visual_cache, ws);
  }
}

void moveCursorLeft(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings) {
  if (buffer->cur_x > 0) {
    buffer->cur_x--;
  } else if (buffer->cur_y > 0) {
    bufferSaveCurrentLine(buffer);
    buffer->cur_y--;
    bufferLoadCurLine(buffer);
    int prevLineLen = strlen(buffer->cur_line);
    if (prevLineLen >= 2 && buffer->cur_line[prevLineLen - 2] == '\r' &&
        buffer->cur_line[prevLineLen - 1] == '\n') {
      prevLineLen -= 2;
    } else if (prevLineLen >= 1 &&
               (buffer->cur_line[prevLineLen - 1] == '\r' ||
                buffer->cur_line[prevLineLen - 1] == '\n')) {
      prevLineLen -= 1;
    }
    buffer->cur_x = prevLineLen;
  }
  screen_settings->logical_wanted_x = buffer->cur_x;
}

void moveCursorUp(struct TextBuffer *buffer,
                  struct ScreenSettings *screen_settings) {
  if (buffer->cur_y == 0)
    return;

  bufferSaveCurrentLine(buffer);

  buffer->cur_y--;

  bufferLoadCurLine(buffer);

  int lineLen = strlen(buffer->cur_line) - countNewLineChars(buffer->cur_line);
  buffer->cur_x = MIN(lineLen, screen_settings->logical_wanted_x);
}

void moveCursorDown(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings,
                    struct VisualCache *visual_cache,
                    struct WindowSettings *ws) {
  if (buffer->cur_y < buffer->lines_num) {
    bufferSaveCurrentLine(buffer);

    // Hitting the virtual line
    if (buffer->cur_y == buffer->lines_num - 1) {

      bufferLoadCurLine(buffer);

      int len = strlen(buffer->cur_line);
      // i need to handle go to the virtual line also as the 'enter' press. so
      // put to the of the line \r\n;
      if (countNewLineChars(buffer->cur_line) < 2) {
        buffer->cur_x = len;
        curLineWriteChar(buffer, '\r');
        curLineWriteChar(buffer, '\n');
        bufferSaveCurrentLine(buffer);
        vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
      }

      buffer->cur_y++;
      curLineClearAndResetX(buffer);
      bufferSaveCurrentLine(buffer);
      vcache_schift_add_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
    } else {
      buffer->cur_y++;
      bufferLoadCurLine(buffer);
      int lenCurrentLine =
          strlen(buffer->cur_line) - countNewLineChars(buffer->cur_line);
      buffer->cur_x = MIN(lenCurrentLine, screen_settings->logical_wanted_x);
    }
  }
}

// OUTPUT
void build_prefix_sum(struct VisualCache *vc) {
  vc->prefix_sum_line_heights[0] = 0;
  for (int i = 1; i <= vc->lines_num; i++) {
    vc->prefix_sum_line_heights[i] =
        vc->prefix_sum_line_heights[i - 1] + vc->lines_screen_height[i - 1];
  }
}

void panel_set_bottom_msg(BottomPanelMessage msg) {
  if (msg >= 0 && msg < PANEL_COUNT) {
    panel_current_message = msg;
  }
}

void calculate_screenY_and_first_printline(
    struct TextBuffer *buffer, struct ScreenSettings *screen_settings,
    struct WindowSettings *ws, struct VisualCache *vc) {
  build_prefix_sum(vc);

  int y = buffer->cur_y;
  int line_end_y = vc->prefix_sum_line_heights[y] + vc->lines_screen_height[y];
  int line_height = vc->lines_screen_height[y];

  int first = screen_settings->first_printline;

  // Пока строка не помещается целиком вниз — скроллим вниз
  while ((line_end_y - vc->prefix_sum_line_heights[first]) >
             ws->screen_height &&
         first < y) {
    first++;
  }

  // Пока строка не помещается целиком вверх — скроллим вверх
  while ((line_end_y - vc->prefix_sum_line_heights[first]) < line_height &&
         first > 0) {
    first--;
  }

  screen_settings->first_printline = first;
}

void screen_buffer_ensure_size(struct ScreenBuffer *screen_buffer, int req_y) {

  if (req_y >= screen_buffer->size) {
    while (req_y >= screen_buffer->size) {
      screen_buffer->size *= 2;
    }
    char *temp = realloc(screen_buffer->content, screen_buffer->size);
    if (!temp) {
      die("screenBufferWriteLine: realloc failed");
    }
    screen_buffer->content = temp;
  }
}

void screen_buffer_write_line(const char *line,
                              struct ScreenBuffer *screen_buffer, int *rows_num,
                              int max_rows, int screen_width) {
  if (line == NULL)
    return;
  int len = strlen(line);
  int offset = 0;

  while (offset < len && *rows_num < max_rows) {
    int remain = len - offset;
    int lineLength = (remain > screen_width) ? screen_width : remain;

    screen_buffer_ensure_size(screen_buffer,
                              screen_buffer->appended + lineLength + 2);

    memcpy(&screen_buffer->content[screen_buffer->appended], &line[offset],
           lineLength);
    screen_buffer->appended += lineLength;

    if (remain >
        screen_width) { // If the line was wrapped, add \r\n for display
      screen_buffer->content[screen_buffer->appended++] = '\r';
      screen_buffer->content[screen_buffer->appended++] = '\n';
    }

    offset += lineLength;
    (*rows_num)++;
  }
}

void screen_buffer_write_bottom_panel(struct WindowSettings *ws,
                                      struct ScreenBuffer *screen_buffer) {
  int panel_rows_num = 0;
  screen_buffer_write_line(panel_bottom_messages[panel_current_message],
                           screen_buffer, &panel_rows_num, ws->bottom_offset,
                           ws->screen_width);
}

char *editor_prepare_screen_buffer(struct TextBuffer *buffer,
                                   struct WindowSettings *ws,
                                   struct ScreenSettings *screen_settings) {
  struct ScreenBuffer screen_buffer = screen_buffer_init();

  for (int i = screen_settings->first_printline;
       i <= buffer->lines_num && screen_buffer.rows_num < ws->screen_height;
       i++) {
    screen_buffer_write_line(buffer->lines[i], &screen_buffer,
                             &screen_buffer.rows_num, ws->screen_height,
                             ws->screen_width);
  }

  screen_buffer.content[screen_buffer.appended] = '\0';

  return screen_buffer.content;
}

char *editor_prepare_panel_screen(struct WindowSettings *ws) {
  struct ScreenBuffer screen_buffer = screen_buffer_init();

  screen_buffer_write_bottom_panel(ws, &screen_buffer);

  screen_buffer.content[screen_buffer.appended] = '\0';

  return screen_buffer.content;
}

void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws,
                         struct ScreenSettings *screen_settings) {

  // clear
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // put cursor to the begining
  write(STDOUT_FILENO, "\x1b[H", 3);

  char *sb_text = editor_prepare_screen_buffer(buffer, ws, screen_settings);
  write(STDOUT_FILENO, sb_text, strlen(sb_text));

  char *sb_panel = editor_prepare_panel_screen(ws);
  int panel_begin_y = ws->terminal_height - ws->bottom_offset +
                      1; // cursor indexed startin from 1

  char *y = makeStringFromInt(panel_begin_y);
  int len = 6 + strlen(y);
  char *str = malloc(len + 1);

  if (!str) {
    free(sb_text);
    free(sb_panel);
    free(y);
    die("editorRefreshScreen: malloc failed");
  }

  strcpy(str, "\x1b[");
  strcat(str, y);
  strcat(str, ";1H");

  write(STDOUT_FILENO, str, len);
  write(STDOUT_FILENO, sb_panel, strlen(sb_panel));

  free(y);
  free(str);

  free(sb_text);
  free(sb_panel);
}

void editorOutputBufferText(struct TextBuffer *buffer) {
  // clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // put screen_settings to the top
  write(STDOUT_FILENO, "\x1b[H", 3);
  for (int i = 0; i < buffer->lines_num; i++) {
    char *str = buffer->lines[i];
    while (*str != '\0') {
      write(STDOUT_FILENO, str, 1);
      str++;
    }
  }
  write(STDOUT_FILENO, "p pressed!", 10);
  sleep(1);
}

// VISUAL_CACHE

void visual_cache_ensure_line_capacity(struct VisualCache *visual_cache,
                                       int index_to_check) {

  if (index_to_check >= visual_cache->lines_capacity) {

    assert(visual_cache->lines_capacity > 0);

    int new_capacity = visual_cache->lines_capacity;

    while (index_to_check >= new_capacity) {
      new_capacity *= 2;
    }

    int *new_array =
        realloc(visual_cache->lines_screen_height, new_capacity * sizeof(int));

    if (!new_array) {
      free(visual_cache->lines_screen_height);
      die("addLineVisualCache: failed realloc");
    } else {
      visual_cache->lines_screen_height = new_array;
      visual_cache->lines_capacity = new_capacity;
    }
  }
}

void vcache_write_line(struct VisualCache *visual_cache,
                       struct WindowSettings *ws, int cur_y, char *line) {
  visual_cache_ensure_line_capacity(visual_cache, cur_y);
  visual_cache->lines_screen_height[cur_y] =
      getScreenLinesForString(line, ws->screen_width);
}

void vcache_schift_add_line(struct VisualCache *visual_cache,
                            struct WindowSettings *ws, int cur_y, char *line) {
  // ensure enough memory
  visual_cache_ensure_line_capacity(visual_cache, visual_cache->lines_num + 1);

  moveIntsDown(visual_cache->lines_screen_height, cur_y,
               &visual_cache->lines_num);
  visual_cache->lines_screen_height[cur_y] =
      getScreenLinesForString(line, ws->screen_width);
}

void vcache_rmv_line(struct VisualCache *visual_cache, int cur_y) {
  moveIntsUp(visual_cache->lines_screen_height, &visual_cache->lines_num,
             cur_y);
}

// FILE ACTIONS

void write_content_in_buffer(char *content, int content_size,
                             struct TextBuffer *buffer,
                             struct WindowSettings *ws,
                             struct VisualCache *visual_cache) {
  if (content == NULL || content_size == 0)
    return;

  int c = 0;

  for (; c < content_size; c++) {
    if (buffer->cur_y == buffer->lines_num) {
      buffer->lines_num++;
      visual_cache->lines_num++;
    }

    if (content[c] == '\n') {
      if (c > 0 && content[c - 1] != '\r') {
        curLineWriteChar(buffer, '\r');
      }
      curLineWriteChar(buffer, '\n');
      bufferSaveCurrentLine(buffer);
      vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
      buffer->cur_y++;
      curLineClearAndResetX(buffer);
    } else {
      curLineWriteChar(buffer, content[c]);
    }
    vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
  }
  buffer->cur_y = 0;
  buffer->cur_x = 0;
}

char *read_file(size_t *size) {
  char *buffer = NULL;

  FILE *f = fopen(input_file_path, "r+");
  if (f == NULL) {
    goto error;
  }

  if (fseek(f, 0, SEEK_END) < 0) {
    goto error;
  }

  long m = ftell(f);
  if (m < 0) {
    goto error;
  }

  buffer = malloc(sizeof(char) * m);
  if (buffer == NULL) {
    goto error;
  }

  if (fseek(f, 0, SEEK_SET) < 0) {
    goto error;
  }

  size_t n = fread(buffer, 1, m, f);
  assert(n == (size_t)m);

  if (ferror(f)) {
    goto error;
  }

  if (size) {
    *size = n;
  }

  fclose(f);

  return buffer;

error:
  if (f) {
    fclose(f);
  }

  if (buffer) {
    free(buffer);
  }

  die("ERROR: read_file failure");

  return NULL;
}

void write_file(struct TextBuffer *buffer) {
  FILE *f = fopen(input_file_path, "w");

  if (f == NULL) {
    goto error;
  }

  for (int y = 0; y < buffer->lines_num; ++y) {
    char *line = buffer->lines[y];
    size_t len = strlen(line);
    size_t written = fwrite(line, sizeof(char), len, f);

    if (written != len) {
      goto error;
    }
  }

  if (buffer->lines[buffer->lines_num] != NULL) {
    char *line = buffer->lines[buffer->lines_num];
    size_t len = strlen(line);
    size_t written = fwrite(line, sizeof(char), len, f);

    if (written != len) {
      goto error;
    }
  }

error:
  if (f) {
    fclose(f);
  }

  die("ERROR: write_file failure");
  return;
}

// INPUT

void bufferLoadCurLine(struct TextBuffer *buffer) {
  if (buffer->cur_y < buffer->lines_num) {
    copyLine(buffer->lines[buffer->cur_y], buffer->cur_line);
  } else {
    curLineClearAndResetX(buffer);
  }
}

void curLineDeleteChar(struct TextBuffer *buffer,
                       struct ScreenSettings *screen_settings,
                       struct VisualCache *visual_cache,
                       struct WindowSettings *ws) {
  if (buffer->cur_x == 0 && buffer->cur_y > 0) {
    bufferSaveCurrentLine(buffer);

    char *prev_str = buffer->lines[buffer->cur_y - 1];
    int len_prev_str = strlen(prev_str);

    if (countNewLineChars(prev_str) == 2) {
      prev_str[len_prev_str - 2] = '\0';
      len_prev_str -= 2;
    } else if (countNewLineChars(prev_str) == 1) {
      prev_str[len_prev_str - 1] = '\0';
      len_prev_str -= 1;
    }

    char *appended_line =
        appendTwoLines(prev_str, buffer->lines[buffer->cur_y]);

    if (appended_line == NULL) {
      die("curLineDeleteChar: appending of two lines is failed");
    }

    moveRowsUp(buffer->lines, &buffer->lines_num, buffer->cur_y);
    vcache_rmv_line(visual_cache, buffer->cur_y);

    buffer->cur_y--;
    curLineClearAndResetX(buffer);
    curLineWriteChars(buffer, appended_line);
    free(appended_line);

    bufferSaveCurrentLine(buffer);
    vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);

    buffer->cur_x = len_prev_str;
    screen_settings->logical_wanted_x = buffer->cur_x;
  } else if (buffer->cur_x > 0) {
    moveCharsLeft(buffer->cur_line, buffer->cur_x, 1);
    buffer->cur_x--;
    bufferSaveCurrentLine(buffer);
    vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
  }
}

void bufferSaveCurrentLine(struct TextBuffer *buffer) {
  editorEnsureLineCapacity(buffer, buffer->cur_y);

  int size = strlen(buffer->cur_line);

  if (buffer->lines[buffer->cur_y] != NULL) {
    free(buffer->lines[buffer->cur_y]); // Free existing memory
  }
  buffer->lines[buffer->cur_y] = malloc(size + 1);

  if (buffer->lines[buffer->cur_y] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  copyLine(buffer->cur_line, buffer->lines[buffer->cur_y]);
}

char editorReadKey() {
  char nread; // output result code
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  return c;
}

void curLineWriteChar(struct TextBuffer *buffer, char c) {
  if (buffer->cur_x >= SIZELINE - 1)
    die("bufferWriteChar: SIZELINE is exceeded");

  int len = strlen(buffer->cur_line);

  if (buffer->cur_x < len) {
    moveCharsRight(buffer->cur_line, buffer->cur_x, 1);
  }

  buffer->cur_line[buffer->cur_x] = c;

  if (buffer->cur_x == len) {
    buffer->cur_line[buffer->cur_x + 1] = '\0';
  }
  buffer->cur_x++;
}

void curLineWriteChars(struct TextBuffer *buffer, const char *chars) {
  size_t add_len = strlen(chars);
  if (add_len == 0) {
    return; // Nothing to do.
  }

  size_t current_len = strlen(buffer->cur_line);

  if (current_len + add_len >= SIZELINE) {
    die("curLineWriteChars: New text exceeds SIZELINE limit");
  }

  if (buffer->cur_x < (int)current_len) {
    moveCharsRight(buffer->cur_line, buffer->cur_x, add_len);
  }

  memcpy(&buffer->cur_line[buffer->cur_x], chars, add_len);
  buffer->cur_line[current_len + add_len] = '\0';
  buffer->cur_x += add_len;
}

void curLineClearAndResetX(struct TextBuffer *buffer) {
  buffer->cur_x = 0;
  buffer->cur_line[buffer->cur_x] = '\0'; // clear current line;
}

void bufferHandleNewLineInput(struct TextBuffer *buffer,
                              struct ScreenSettings *screen_settings,
                              struct VisualCache *visual_cache,
                              struct WindowSettings *ws) {
  editorEnsureLineCapacity(buffer, buffer->lines_num + 1);

  char **splitted_lines = splitLine(buffer->cur_line, buffer->cur_x);
  if (splitted_lines == NULL) {
    return;
  }

  char *first_half = splitted_lines[0];
  char *second_half = splitted_lines[1];

  moveRowsDown(buffer->lines, buffer->cur_y + 1, &buffer->lines_num);

  curLineClearAndResetX(buffer);

  first_half = addNewLineChar(first_half);

  if (first_half == NULL) {
    die("bufferHandleNewLineInput: malloc failed -> first_half var");
  }

  curLineWriteChars(buffer, first_half);
  bufferSaveCurrentLine(buffer);
  vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);

  buffer->cur_y++;
  curLineClearAndResetX(buffer);
  curLineWriteChars(buffer, second_half);
  bufferSaveCurrentLine(buffer);
  vcache_schift_add_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);

  buffer->cur_x = 0;
  screen_settings->logical_wanted_x = 1;

  free(first_half);
  free(second_half);
  free(splitted_lines);
}

void bufferHandleEscapeSequence(struct TextBuffer *buffer,
                                struct ScreenSettings *screen_settings,
                                struct VisualCache *visual_cache,
                                struct WindowSettings *ws) {
  if (!isInputAvailable())
    return;
  char c = editorReadKey();
  if (c != '[')
    return;
  if (!isInputAvailable())
    return;
  c = editorReadKey();
  switch (c) {
  case 'A':
    moveCursorUp(buffer, screen_settings);
    break;
  case 'B':
    moveCursorDown(buffer, screen_settings, visual_cache, ws);
    break;
  case 'C':
    moveCursorRight(buffer, screen_settings, visual_cache, ws);
    break;
  case 'D':
    moveCursorLeft(buffer, screen_settings);
    break;
  case '<':
    // This is a mouse event. Consume until 'M' or 'm'.
    do {
      if (!isInputAvailable())
        return;
      c = editorReadKey();
    } while (c != 'M' && c != 'm');
    break;
  }
}

void editorHandleQuit(struct TextBuffer *buffer, struct WindowSettings *ws,
                      struct ScreenSettings *screen_settings) {
  panel_set_bottom_msg(PANEL_QUIT_CONFIRM);
  editorRefreshScreen(buffer, ws, screen_settings);

  char c = editorReadKey();

  switch (c) {
  case 'y':
  case 'Y':
    write_file(buffer);
    cleanEditor();
    exit(0);
  case 'n':
  case 'N':
    cleanEditor();
    exit(0);
    break;
  default:
    while (isInputAvailable()) {
      editorReadKey();
    }
    panel_set_bottom_msg(PANEL_DEFAULT);
    break;
  }
}

void editorProcessKeypress(struct TextBuffer *buffer, struct WindowSettings *ws,
                           struct ScreenSettings *screen_settings,
                           struct VisualCache *visual_cache) {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    editorHandleQuit(buffer, ws, screen_settings);
    break;
  case ('\r'):
  case ('\n'):
    bufferHandleNewLineInput(buffer, screen_settings, visual_cache, ws);
    break;
  case ('\t'):
    curLineWriteChar(buffer, ' ');
    curLineWriteChar(buffer, ' ');
    curLineWriteChar(buffer, ' ');
    break;
  case CTRL_KEY('p'):
    editorOutputBufferText(buffer);
    break;
  case DEL:
  case BACKSPACE:
    curLineDeleteChar(buffer, screen_settings, visual_cache, ws);
    break;
  case ('\x1b'):
    bufferHandleEscapeSequence(buffer, screen_settings, visual_cache, ws);
    break;
  default:
    if (buffer->cur_y == buffer->lines_num) {
      buffer->lines_num++;
      visual_cache->lines_num++;
    }

    if (screen_settings->first_printline < 0 || screen_settings->cursor_y < 0) {
      write(STDOUT_FILENO, "shit", 4);
      sleep(1);
    }

    curLineWriteChar(buffer, c);
    bufferSaveCurrentLine(buffer);
    vcache_write_line(visual_cache, ws, buffer->cur_y, buffer->cur_line);
    screen_settings->logical_wanted_x = buffer->cur_x;
    break;
  }

  editorUpdateCursorCoordinates(buffer, ws, screen_settings, visual_cache);
  editorRefreshScreen(buffer, ws, screen_settings);
  editorRefreshCursor(screen_settings);
}

// INIT
int main(int argc, char **argv) {

  if (argc < 2) {
    fprintf(stderr, "ERROR: input file is not provided\n");
    exit(1);
  }

  input_file_path = argv[1];

  switchToAlternateScreen();
  enableRawMode();
  struct TextBuffer buffer = textBufferInit();
  struct WindowSettings ws = windowSettingsInit();
  struct ScreenSettings screen_settings = {1, 1, 1, 0};
  struct VisualCache visual_cache = visualCacheInit();

  if (access(input_file_path, F_OK) == 0) {
    // file exists
    size_t content_size = 0;
    char *file_content = read_file(&content_size);

    if (file_content == NULL) {
      fprintf(stderr, "ERROR: could not read file %s: %s\n", input_file_path,
              strerror(errno));
      exit(1);
    }
    write_content_in_buffer(file_content, content_size, &buffer, &ws,
                            &visual_cache);
    free(file_content);
    bufferLoadCurLine(&buffer);
  }

  editorUpdateCursorCoordinates(&buffer, &ws, &screen_settings, &visual_cache);
  editorRefreshScreen(&buffer, &ws, &screen_settings);
  editorRefreshCursor(&screen_settings);
  while (1) {
    editorProcessKeypress(&buffer, &ws, &screen_settings, &visual_cache);
  }

  return 0;
}
