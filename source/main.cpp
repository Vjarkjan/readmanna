#include <nds.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fat.h>

namespace {

constexpr int LOGICAL_HEIGHT = SCREEN_HEIGHT * 2;

constexpr int PAGE_WIDTH = 960;
constexpr int PAGE_HEIGHT = 1440;
constexpr int PAGE_PIXELS = PAGE_WIDTH * PAGE_HEIGHT;
constexpr size_t PAGE_BYTES = static_cast<size_t>(PAGE_PIXELS) * sizeof(u16);
constexpr size_t MAX_COMPRESSED_ASSET_BYTES =
    8 + PAGE_BYTES + (PAGE_BYTES + 7) / 8 + 4;

constexpr int PREVIEW_WIDTH = SCREEN_WIDTH;
constexpr int PREVIEW_HEIGHT = LOGICAL_HEIGHT;
constexpr int PREVIEW_PIXELS = PREVIEW_WIDTH * PREVIEW_HEIGHT;
constexpr size_t PREVIEW_BYTES = static_cast<size_t>(PREVIEW_PIXELS) * sizeof(u16);

constexpr int MAX_CHAPTERS = 256;
constexpr int MAX_BOOKS = 128;
constexpr int BOOK_FOLDER_CAPACITY = 49;
constexpr int BOOK_TITLE_CAPACITY = 49;
constexpr int PATH_CAPACITY = 192;
constexpr int PAN_STEP_PIXELS = 8;
constexpr int PAGE_INPUT_CAPACITY = 11;
constexpr int BOOK_PICKER_ROWS = 16;
constexpr int BOOK_SHORTCUT_GRACE_FRAMES = 4;
constexpr u32 BOOK_PICKER_CHORD = KEY_START | KEY_SELECT;
constexpr const char* MANIFEST_MAGIC = "DS_MANGAMAN_BOOK_V3_ASSET";
constexpr char LAST_POSITION_PREFIX[] = "last_position=";
constexpr size_t LAST_POSITION_PREFIX_LENGTH = sizeof(LAST_POSITION_PREFIX) - 1;
constexpr size_t LEGACY_LAST_POSITION_LINE_LENGTH =
    LAST_POSITION_PREFIX_LENGTH + 4 + 1 + 10;
// New records append quarter-turn rotation and the stable four-digit crop
// width: last_position=CCCC:PPPPPPPPPP:R:WWWW
constexpr size_t LAST_POSITION_LINE_LENGTH =
    LEGACY_LAST_POSITION_LINE_LENGTH + 1 + 1 + 1 + 4;
constexpr size_t LAST_POSITION_RECORD_BYTES = LAST_POSITION_LINE_LENGTH + 1;
constexpr const char* BOOKS_ROOT = "/readmanna/books";
constexpr size_t ASSET_MAGIC_SIZE = 8;
constexpr size_t ASSET_PALETTE_BYTES = 256 * sizeof(u16);
constexpr char ASSET_MAGIC_PALETTE8[] = "DSMP8L10";
constexpr char ASSET_MAGIC_RGB555[] = "DSM16L10";

struct Chapter {
    int number;
    int page_count;
};

struct Book {
    char folder[BOOK_FOLDER_CAPACITY];
    char title[BOOK_TITLE_CAPACITY];
};

struct CropSize {
    int width;
    int height;
};

struct ImageSize {
    int width;
    int height;
};

struct DragState {
    bool active;
    int touch_x;
    int touch_y;
    int starting_view_x;
    int starting_view_y;
};

// Every crop keeps the source's 2:3 aspect ratio, matching the combined 256x384 LCD area.
constexpr CropSize ZOOM_CROPS[] = {
    {960, 1440},
    {768, 1152},
    {640, 960},
    {512, 768},
    {480, 720},
    {384, 576},
    {320, 480},
};
constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_CROPS) / sizeof(ZOOM_CROPS[0]);
constexpr int FIRST_FILTERED_ZOOM = 1;
constexpr int LAST_FILTERED_ZOOM = ZOOM_LEVEL_COUNT - 1;
constexpr int FILTERED_ZOOM_3X_LEVEL = 1;  // 960x1440 -> 320x480
constexpr int FILTERED_ZOOM_2X_LEVEL = 3;  // 960x1440 -> 480x720
// Zoom levels 3 and 4 use exact host-generated caches.  The exporter creates
// both textures on the PC so entering either middle zoom only needs an SD read
// plus BIOS LZ10 decompression instead of a DSi-side resize.
constexpr int PREPROCESSED_ZOOM480_LEVEL = 3;
constexpr int PREPROCESSED_ZOOM480_WIDTH = 480;
constexpr int PREPROCESSED_ZOOM480_HEIGHT = 720;
constexpr int PREPROCESSED_ZOOM480_PIXELS =
    PREPROCESSED_ZOOM480_WIDTH * PREPROCESSED_ZOOM480_HEIGHT;
constexpr size_t PREPROCESSED_ZOOM480_BYTES =
    static_cast<size_t>(PREPROCESSED_ZOOM480_PIXELS) * sizeof(u16);
constexpr int PREPROCESSED_ZOOM512_LEVEL = 4;
constexpr int PREPROCESSED_ZOOM512_WIDTH = 512;
constexpr int PREPROCESSED_ZOOM512_HEIGHT = 768;
constexpr int PREPROCESSED_ZOOM512_PIXELS =
    PREPROCESSED_ZOOM512_WIDTH * PREPROCESSED_ZOOM512_HEIGHT;
constexpr size_t PREPROCESSED_ZOOM512_BYTES =
    static_cast<size_t>(PREPROCESSED_ZOOM512_PIXELS) * sizeof(u16);
constexpr int PREPROCESSED_ZOOM_SCRATCH_PIXELS =
    PREPROCESSED_ZOOM512_PIXELS;
constexpr int MAX_ZOOM_TEXTURE_WIDTH = 768;
constexpr int MAX_ZOOM_TEXTURE_HEIGHT = 1152;
constexpr int MAX_ZOOM_TEXTURE_PIXELS =
    MAX_ZOOM_TEXTURE_WIDTH * MAX_ZOOM_TEXTURE_HEIGHT;
constexpr int MAX_AREA_FILTER_TAPS = 4;

struct AreaFilterSpan {
    int first;
    int count;
    u16 weights[MAX_AREA_FILTER_TAPS];
};

struct RotatedTextureLayout {
    int width;
    int height;
    int origin;
    int x_stride;
    int y_stride;
};

// These two full-page buffers require DSi RAM. Loading into the inactive buffer keeps
// the current page intact if the SD card contains a missing, truncated, or oversized asset.
static u16 page_buffers[2][PAGE_PIXELS] TWL_BSS __attribute__((aligned(32)));
static u16 preview_buffers[2][PREVIEW_PIXELS] TWL_BSS __attribute__((aligned(32)));
// Quarter-turn rendering stays fast by rotating the small active preview once,
// then reading it as contiguous rows just like the upright preview.
static u16 rotated_preview_buffer[PREVIEW_PIXELS]
    TWL_BSS __attribute__((aligned(32)));
// Detailed zooms use one page-specific area-filtered texture. It is built only
// after the candidate page assets have loaded successfully, so failed SD reads
// leave the active page and its cache intact without reserving a second cache.
static u16 zoom_texture_buffer[MAX_ZOOM_TEXTURE_PIXELS]
    TWL_BSS __attribute__((aligned(32)));
// Scratch space is used only when a preprocessed middle-zoom texture must be
// physically rotated.  The 512x768 asset is the larger of the two, so one
// buffer serves both 480x720 and 512x768 without increasing DSi RAM usage.
static u16 preprocessed_zoom_buffer[PREPROCESSED_ZOOM_SCRATCH_PIXELS]
    TWL_BSS __attribute__((aligned(32)));
static AreaFilterSpan horizontal_filter_spans[MAX_ZOOM_TEXTURE_WIDTH]
    TWL_BSS __attribute__((aligned(32)));
static AreaFilterSpan vertical_filter_spans[MAX_ZOOM_TEXTURE_HEIGHT]
    TWL_BSS __attribute__((aligned(32)));
// Tiny coordinate/offset tables are touched for every pixel in the bilinear
// fallback, so keep them in ARM9 DTCM.  They replace per-pixel division,
// rotation switches, and source-address multiplication.
static s16 bilinear_x_coordinates[SCREEN_WIDTH] DTCM_BSS;
static s32 bilinear_x_offsets[SCREEN_WIDTH] DTCM_BSS;
static u8 bilinear_x_fractions[SCREEN_WIDTH] DTCM_BSS;
static s16 bilinear_y_coordinates[SCREEN_HEIGHT] DTCM_BSS;
static s32 bilinear_y_offsets[SCREEN_HEIGHT] DTCM_BSS;
static u8 bilinear_y_fractions[SCREEN_HEIGHT] DTCM_BSS;

static u8 compressed_asset_buffer[MAX_COMPRESSED_ASSET_BYTES]
    TWL_BSS __attribute__((aligned(32)));
static u16 top_screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(32)));
static u16 bottom_screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT] __attribute__((aligned(32)));

static Chapter chapters[MAX_CHAPTERS];
static Chapter manifest_scratch[MAX_CHAPTERS];
static Book books[MAX_BOOKS];
static int chapter_count = 0;
static int book_count = 0;
static int current_book_index = -1;
static int current_chapter_index = 0;
static int current_page = 1;
static int active_page_buffer = 0;
static bool full_page_loaded[2] = {};
static int rotated_preview_page_buffer = -1;
static int rotated_preview_rotation = -1;
static int zoom_texture_level = -1;
static int zoom_texture_page_buffer = -1;
static int zoom_texture_rotation = -1;
static char current_book_path[PATH_CAPACITY];

static int zoom_index = 0;
static int view_x = 0;
static int view_y = 0;
// 0, 1, 2, and 3 represent 0, 90, 180, and 270 degrees clockwise.
static int rotation_quarters_clockwise = 0;

static int top_background = -1;
static int bottom_background = -1;

constexpr u16 opaque_rgb(int red, int green, int blue) {
    return static_cast<u16>(RGB15(red, green, blue) | BIT(15));
}

constexpr u16 COLOR_BLACK = opaque_rgb(0, 0, 0);
constexpr u16 COLOR_WHITE = opaque_rgb(31, 31, 31);
constexpr u16 COLOR_PANEL = opaque_rgb(4, 4, 5);
constexpr u16 COLOR_BUTTON = opaque_rgb(10, 10, 12);
constexpr u16 COLOR_ACCENT = opaque_rgb(8, 21, 31);
constexpr u16 COLOR_ERROR = opaque_rgb(31, 5, 5);

void fatal_error(const char* title, const char* detail = nullptr) {
    consoleDemoInit();
    iprintf("Readmanna\n\n%s\n", title);
    if (detail != nullptr) {
        iprintf("\n%s\n", detail);
    }
    iprintf("\nTap POWER to return to the launcher.");

    while (pmMainLoop()) {
        swiWaitForVBlank();
    }
    exit(0);
}

int read_manifest_line(FILE* file, char* buffer, size_t capacity) {
    if (fgets(buffer, static_cast<int>(capacity), file) == nullptr) {
        return feof(file) ? 0 : -1;
    }

    size_t length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == '\n') {
        buffer[--length] = '\0';
        if (length > 0 && buffer[length - 1] == '\r') {
            buffer[length - 1] = '\0';
        }
    } else if (!feof(file)) {
        // The line did not fit. Consume it so a caller cannot parse a partial record.
        int character = 0;
        do {
            character = fgetc(file);
        } while (character != '\n' && character != EOF);
        return -1;
    }

    return 1;
}

bool parse_integer_pair(const char* line, long* first, long* second) {
    const char* cursor = line;
    while (isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }

    errno = 0;
    char* end = nullptr;
    const long parsed_first = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE) {
        return false;
    }

    cursor = end;
    while (isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }

    errno = 0;
    const long parsed_second = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE) {
        return false;
    }

    cursor = end;
    while (isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    if (*cursor != '\0') {
        return false;
    }

    *first = parsed_first;
    *second = parsed_second;
    return true;
}

bool parse_integer_quad(
    const char* line,
    long* first,
    long* second,
    long* third,
    long* fourth
) {
    long* outputs[] = {first, second, third, fourth};
    const char* cursor = line;

    for (long* output : outputs) {
        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }

        errno = 0;
        char* end = nullptr;
        const long parsed = strtol(cursor, &end, 10);
        if (end == cursor || errno == ERANGE) {
            return false;
        }

        *output = parsed;
        cursor = end;
    }

    while (isspace(static_cast<unsigned char>(*cursor))) {
        ++cursor;
    }
    return *cursor == '\0';
}

bool valid_book_folder(const char* folder) {
    const size_t length = strlen(folder);
    if (length == 0 || length >= BOOK_FOLDER_CAPACITY) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(folder[index]);
        if (!isalnum(character) && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

bool valid_book_title(const char* title) {
    const size_t length = strlen(title);
    if (length == 0 || length >= BOOK_TITLE_CAPACITY ||
        title[0] == ' ' || title[length - 1] == ' ') {
        return false;
    }

    bool contains_visible_character = false;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(title[index]);
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
        if (character != ' ') {
            contains_visible_character = true;
        }
    }
    return contains_visible_character;
}

bool parse_last_position_line(
    const char* line,
    int* chapter_number,
    int* page_number,
    int* rotation,
    int* zoom_level
) {
    if (line == nullptr || chapter_number == nullptr || page_number == nullptr ||
        rotation == nullptr || zoom_level == nullptr) {
        return false;
    }

    const size_t line_length = strlen(line);
    const bool legacy_record =
        line_length == LEGACY_LAST_POSITION_LINE_LENGTH;
    if ((!legacy_record && line_length != LAST_POSITION_LINE_LENGTH) ||
        memcmp(line, LAST_POSITION_PREFIX, LAST_POSITION_PREFIX_LENGTH) != 0 ||
        line[LAST_POSITION_PREFIX_LENGTH + 4] != ':') {
        return false;
    }

    int parsed_chapter = 0;
    for (size_t index = 0; index < 4; ++index) {
        const char character = line[LAST_POSITION_PREFIX_LENGTH + index];
        if (character < '0' || character > '9') {
            return false;
        }
        parsed_chapter = parsed_chapter * 10 + character - '0';
    }

    int parsed_page = 0;
    const size_t page_offset = LAST_POSITION_PREFIX_LENGTH + 5;
    for (size_t index = 0; index < 10; ++index) {
        const char character = line[page_offset + index];
        if (character < '0' || character > '9') {
            return false;
        }
        const int digit = character - '0';
        if (parsed_page > (INT_MAX - digit) / 10) {
            return false;
        }
        parsed_page = parsed_page * 10 + digit;
    }
    if (parsed_page < 1) {
        return false;
    }

    int parsed_rotation = 0;
    int parsed_zoom_level = 0;
    if (!legacy_record) {
        const size_t rotation_separator_offset =
            LEGACY_LAST_POSITION_LINE_LENGTH;
        const size_t rotation_offset = rotation_separator_offset + 1;
        const size_t zoom_separator_offset = rotation_offset + 1;
        const size_t zoom_width_offset = zoom_separator_offset + 1;
        if (line[rotation_separator_offset] != ':' ||
            line[zoom_separator_offset] != ':' ||
            line[rotation_offset] < '0' || line[rotation_offset] > '3') {
            return false;
        }
        parsed_rotation = line[rotation_offset] - '0';

        int parsed_crop_width = 0;
        for (size_t index = 0; index < 4; ++index) {
            const char character = line[zoom_width_offset + index];
            if (character < '0' || character > '9') {
                return false;
            }
            parsed_crop_width =
                parsed_crop_width * 10 + character - '0';
        }

        parsed_zoom_level = -1;
        for (int level = 0; level < ZOOM_LEVEL_COUNT; ++level) {
            if (ZOOM_CROPS[level].width == parsed_crop_width) {
                parsed_zoom_level = level;
                break;
            }
        }
        if (parsed_zoom_level < 0) {
            return false;
        }
    }

    *chapter_number = parsed_chapter;
    *page_number = parsed_page;
    *rotation = parsed_rotation;
    *zoom_level = parsed_zoom_level;
    return true;
}

bool parse_book_manifest(
    const char* book_path,
    char* title_output,
    size_t title_capacity,
    Chapter* chapter_output,
    int* parsed_chapter_count,
    int* resume_chapter_index_output = nullptr,
    int* resume_page_output = nullptr,
    int* resume_rotation_output = nullptr,
    int* resume_zoom_level_output = nullptr
) {
    if (book_path == nullptr || title_output == nullptr || title_capacity == 0 ||
        chapter_output == nullptr || parsed_chapter_count == nullptr) {
        return false;
    }

    *parsed_chapter_count = 0;
    title_output[0] = '\0';
    if (resume_chapter_index_output != nullptr) {
        *resume_chapter_index_output = 0;
    }
    if (resume_page_output != nullptr) {
        *resume_page_output = 1;
    }
    if (resume_rotation_output != nullptr) {
        *resume_rotation_output = 0;
    }
    if (resume_zoom_level_output != nullptr) {
        *resume_zoom_level_output = 0;
    }

    char manifest_path[PATH_CAPACITY];
    const int path_length = snprintf(
        manifest_path,
        sizeof(manifest_path),
        "%s/book.cfg",
        book_path
    );
    if (path_length < 0 || path_length >= static_cast<int>(sizeof(manifest_path))) {
        return false;
    }

    FILE* file = fopen(manifest_path, "rb");
    if (file == nullptr) {
        return false;
    }

    char line[128];
    bool valid = true;
    long width = 0;
    long height = 0;
    long preview_width = 0;
    long preview_height = 0;

    if (read_manifest_line(file, line, sizeof(line)) != 1 ||
        strcmp(line, MANIFEST_MAGIC) != 0) {
        valid = false;
    }

    if (valid && read_manifest_line(file, line, sizeof(line)) != 1) {
        valid = false;
    }
    if (valid && (!parse_integer_quad(
            line,
            &width,
            &height,
            &preview_width,
            &preview_height
        ) ||
        width != PAGE_WIDTH || height != PAGE_HEIGHT ||
        preview_width != PREVIEW_WIDTH || preview_height != PREVIEW_HEIGHT)) {
        valid = false;
    }

    if (valid && read_manifest_line(file, line, sizeof(line)) != 1) {
        valid = false;
    }
    if (valid && (!valid_book_title(line) || strlen(line) >= title_capacity)) {
        valid = false;
    }
    if (valid) {
        strcpy(title_output, line);
    }

    int count = 0;
    int resume_chapter_number = -1;
    int resume_page_number = -1;
    int resume_rotation = 0;
    int resume_zoom_level = 0;
    while (valid) {
        const int line_result = read_manifest_line(file, line, sizeof(line));
        if (line_result == 0) {
            break;
        }
        if (line_result < 0) {
            valid = false;
            break;
        }

        const char* cursor = line;
        while (isspace(static_cast<unsigned char>(*cursor))) {
            ++cursor;
        }
        if (*cursor == '\0') {
            continue;
        }

        if (strncmp(
                cursor,
                LAST_POSITION_PREFIX,
                LAST_POSITION_PREFIX_LENGTH
            ) == 0) {
            int parsed_resume_chapter = 0;
            int parsed_resume_page = 0;
            int parsed_resume_rotation = 0;
            int parsed_resume_zoom_level = 0;
            if (parse_last_position_line(
                    cursor,
                    &parsed_resume_chapter,
                    &parsed_resume_page,
                    &parsed_resume_rotation,
                    &parsed_resume_zoom_level
                )) {
                // If a recoverable interrupted update left more than one resume
                // record, the final valid record is the newest one.
                resume_chapter_number = parsed_resume_chapter;
                resume_page_number = parsed_resume_page;
                resume_rotation = parsed_resume_rotation;
                resume_zoom_level = parsed_resume_zoom_level;
            }
            // A damaged resume record never makes the book itself unreadable.
            continue;
        }

        long chapter_number = 0;
        long pages = 0;
        if (!parse_integer_pair(line, &chapter_number, &pages) ||
            chapter_number < 0 || chapter_number > 9999 ||
            pages < 1 || pages > INT_MAX || count >= MAX_CHAPTERS ||
            (count > 0 && chapter_number <= chapter_output[count - 1].number)) {
            valid = false;
            break;
        }

        chapter_output[count].number = static_cast<int>(chapter_number);
        chapter_output[count].page_count = static_cast<int>(pages);
        ++count;
    }

    if (ferror(file)) {
        valid = false;
    }
    fclose(file);

    if (!valid || count == 0) {
        title_output[0] = '\0';
        return false;
    }

    *parsed_chapter_count = count;
    if (resume_chapter_index_output != nullptr || resume_page_output != nullptr ||
        resume_rotation_output != nullptr ||
        resume_zoom_level_output != nullptr) {
        for (int index = 0; index < count; ++index) {
            if (chapter_output[index].number == resume_chapter_number &&
                resume_page_number >= 1 &&
                resume_page_number <= chapter_output[index].page_count) {
                if (resume_chapter_index_output != nullptr) {
                    *resume_chapter_index_output = index;
                }
                if (resume_page_output != nullptr) {
                    *resume_page_output = resume_page_number;
                }
                if (resume_rotation_output != nullptr) {
                    *resume_rotation_output = resume_rotation;
                }
                if (resume_zoom_level_output != nullptr) {
                    *resume_zoom_level_output = resume_zoom_level;
                }
                break;
            }
        }
    }
    return true;
}

int compare_ascii_case_insensitive(const char* first, const char* second) {
    while (*first != '\0' && *second != '\0') {
        const int first_lower = tolower(static_cast<unsigned char>(*first));
        const int second_lower = tolower(static_cast<unsigned char>(*second));
        if (first_lower != second_lower) {
            return first_lower - second_lower;
        }
        ++first;
        ++second;
    }
    return static_cast<unsigned char>(*first) - static_cast<unsigned char>(*second);
}

int compare_books(const void* first_pointer, const void* second_pointer) {
    const Book* first = static_cast<const Book*>(first_pointer);
    const Book* second = static_cast<const Book*>(second_pointer);
    const int title_comparison = compare_ascii_case_insensitive(first->title, second->title);
    return title_comparison != 0 ? title_comparison : strcmp(first->folder, second->folder);
}

bool scan_books() {
    book_count = 0;
    DIR* directory = opendir(BOOKS_ROOT);
    if (directory == nullptr) {
        return false;
    }

    while (book_count < MAX_BOOKS) {
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            break;
        }
        if (!valid_book_folder(entry->d_name)) {
            continue;
        }

        char book_path[PATH_CAPACITY];
        const int path_length = snprintf(
            book_path,
            sizeof(book_path),
            "%s/%s",
            BOOKS_ROOT,
            entry->d_name
        );
        if (path_length < 0 || path_length >= static_cast<int>(sizeof(book_path))) {
            continue;
        }

        int parsed_count = 0;
        char title[BOOK_TITLE_CAPACITY];
        if (!parse_book_manifest(
                book_path,
                title,
                sizeof(title),
                manifest_scratch,
                &parsed_count
            )) {
            continue;
        }

        strcpy(books[book_count].folder, entry->d_name);
        strcpy(books[book_count].title, title);
        ++book_count;
    }

    closedir(directory);
    if (book_count > 1) {
        qsort(books, book_count, sizeof(books[0]), compare_books);
    }
    return book_count > 0;
}

void draw_book_picker(int selected_book, bool allow_cancel) {
    const int first_book = (selected_book / BOOK_PICKER_ROWS) * BOOK_PICKER_ROWS;
    int last_book = first_book + BOOK_PICKER_ROWS;
    if (last_book > book_count) {
        last_book = book_count;
    }

    iprintf("\x1b[2J\x1b[1;1H");
    iprintf("Readmanna\n");
    iprintf("Books %d-%d of %d\n", first_book + 1, last_book, book_count);
    if (allow_cancel) {
        iprintf("D-pad: choose  A: open  B: back\n\n");
    } else {
        iprintf("D-pad: choose       A: open\n\n");
    }

    for (int index = first_book; index < last_book; ++index) {
        iprintf("%c %.29s\n", index == selected_book ? '>' : ' ', books[index].title);
    }
}

int choose_book(int initial_book = 0, bool allow_cancel = false) {
    if (book_count == 1 && !allow_cancel) {
        return 0;
    }

    int selected_book = initial_book;
    if (selected_book < 0 || selected_book >= book_count) {
        selected_book = 0;
    }
    keysSetRepeat(12, 3);
    draw_book_picker(selected_book, allow_cancel);

    while (pmMainLoop()) {
        scanKeys();
        const u32 keys_down = keysDown();
        const u32 repeated_keys = keysDownRepeat();
        int next_book = selected_book;

        if (repeated_keys & KEY_UP) --next_book;
        if (repeated_keys & KEY_DOWN) ++next_book;
        if (repeated_keys & KEY_LEFT) next_book -= BOOK_PICKER_ROWS;
        if (repeated_keys & KEY_RIGHT) next_book += BOOK_PICKER_ROWS;

        if (next_book < 0) next_book = 0;
        if (next_book >= book_count) next_book = book_count - 1;
        if (next_book != selected_book) {
            selected_book = next_book;
            draw_book_picker(selected_book, allow_cancel);
        }

        if (keys_down & KEY_A) {
            return selected_book;
        }
        if (allow_cancel && (keys_down & KEY_B)) {
            return -1;
        }
        swiWaitForVBlank();
    }
    exit(0);
}

bool activate_book(int book_index) {
    if (book_index < 0 || book_index >= book_count) {
        return false;
    }

    const int path_length = snprintf(
        current_book_path,
        sizeof(current_book_path),
        "%s/%s",
        BOOKS_ROOT,
        books[book_index].folder
    );
    if (path_length < 0 || path_length >= static_cast<int>(sizeof(current_book_path))) {
        return false;
    }

    char verified_title[BOOK_TITLE_CAPACITY];
    int verified_chapter_count = 0;
    int resume_chapter_index = 0;
    int resume_page = 1;
    int resume_rotation = 0;
    int resume_zoom_level = 0;
    if (!parse_book_manifest(
            current_book_path,
            verified_title,
            sizeof(verified_title),
            chapters,
            &verified_chapter_count,
            &resume_chapter_index,
            &resume_page,
            &resume_rotation,
            &resume_zoom_level
        )) {
        return false;
    }

    chapter_count = verified_chapter_count;
    current_book_index = book_index;
    current_chapter_index = resume_chapter_index;
    current_page = resume_page;
    rotation_quarters_clockwise = resume_rotation;
    zoom_index = resume_zoom_level;
    return true;
}

bool persist_current_state() {
    if (current_chapter_index < 0 || current_chapter_index >= chapter_count ||
        current_page < 1 ||
        current_page > chapters[current_chapter_index].page_count ||
        rotation_quarters_clockwise < 0 ||
        rotation_quarters_clockwise > 3 ||
        zoom_index < 0 || zoom_index >= ZOOM_LEVEL_COUNT) {
        return false;
    }

    char manifest_path[PATH_CAPACITY];
    const int path_length = snprintf(
        manifest_path,
        sizeof(manifest_path),
        "%s/book.cfg",
        current_book_path
    );
    if (path_length < 0 || path_length >= static_cast<int>(sizeof(manifest_path))) {
        return false;
    }

    char record[LAST_POSITION_RECORD_BYTES + 1];
    const int record_length = snprintf(
        record,
        sizeof(record),
        "%s%04d:%010d:%d:%04d\n",
        LAST_POSITION_PREFIX,
        chapters[current_chapter_index].number,
        current_page,
        rotation_quarters_clockwise,
        ZOOM_CROPS[zoom_index].width
    );
    if (record_length != static_cast<int>(LAST_POSITION_RECORD_BYTES)) {
        return false;
    }

    FILE* file = fopen(manifest_path, "r+b");
    if (file == nullptr) {
        return false;
    }

    // Every committed page, rotation, and zoom change calls this function
    // immediately. The extended record has a fixed size, so normal saves can
    // replace it in place instead of rewriting the chapter manifest.
    long record_offset = -1;
    char line[128];
    bool scan_failed = false;
    while (true) {
        const long line_offset = ftell(file);
        if (line_offset < 0) {
            scan_failed = true;
            break;
        }
        if (fgets(line, sizeof(line), file) == nullptr) {
            scan_failed = ferror(file) != 0;
            break;
        }
        if (strncmp(line, LAST_POSITION_PREFIX, LAST_POSITION_PREFIX_LENGTH) == 0 &&
            strlen(line) == LAST_POSITION_RECORD_BYTES &&
            line[LAST_POSITION_RECORD_BYTES - 1] == '\n') {
            // Keep scanning so an earlier interrupted migration cannot cause a
            // stale duplicate after this record to override the new value.
            record_offset = line_offset;
        }
    }

    clearerr(file);
    bool valid = !scan_failed;
    long write_offset = -1;
    if (valid) {
        if (record_offset >= 0) {
            valid = fseek(file, record_offset, SEEK_SET) == 0;
        } else {
            valid = fseek(file, 0, SEEK_END) == 0;
            const long file_size = valid ? ftell(file) : -1;
            bool needs_newline = false;
            if (valid && file_size > 0) {
                valid = fseek(file, -1, SEEK_END) == 0;
                const int final_character = valid ? fgetc(file) : EOF;
                valid = valid && final_character != EOF;
                needs_newline = valid && final_character != '\n';
            }
            if (valid) {
                valid = fseek(file, 0, SEEK_END) == 0;
            }
            if (valid && needs_newline) {
                valid = fputc('\n', file) != EOF;
            }
        }
    }

    if (valid) {
        write_offset = ftell(file);
        valid = write_offset >= 0;
    }

    if (valid) {
        valid = fwrite(record, 1, LAST_POSITION_RECORD_BYTES, file) ==
            LAST_POSITION_RECORD_BYTES;
    }
    if (valid) {
        valid = fflush(file) == 0;
    }
    if (valid) {
        // Read the just-written fixed-width slot back before closing. This
        // catches short or failed FAT writes while keeping reader controls
        // usable when a card has become read-only.
        char verified_record[LAST_POSITION_RECORD_BYTES];
        valid = fseek(file, write_offset, SEEK_SET) == 0 &&
            fread(
                verified_record,
                1,
                LAST_POSITION_RECORD_BYTES,
                file
            ) == LAST_POSITION_RECORD_BYTES &&
            memcmp(
                verified_record,
                record,
                LAST_POSITION_RECORD_BYTES
            ) == 0;
    }
    if (fclose(file) != 0) {
        valid = false;
    }
    return valid;
}

bool read_compressed_asset_file(
    const char* path,
    size_t* compressed_size
) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }

    bool valid = fseek(file, 0, SEEK_END) == 0;
    const long file_size = valid ? ftell(file) : -1;
    valid = valid && file_size >= 4 &&
        static_cast<unsigned long>(file_size) <= MAX_COMPRESSED_ASSET_BYTES &&
        fseek(file, 0, SEEK_SET) == 0;

    size_t bytes_read = 0;
    if (valid) {
        bytes_read = fread(
            compressed_asset_buffer,
            1,
            static_cast<size_t>(file_size),
            file
        );
        valid = bytes_read == static_cast<size_t>(file_size) && !ferror(file);
    }
    fclose(file);

    if (valid) {
        *compressed_size = bytes_read;
    }
    return valid;
}

bool decompress_lz10_asset(
    const u8* source,
    size_t source_size,
    void* destination_pointer,
    size_t expected_size
) {
    if (source == nullptr || destination_pointer == nullptr || source_size < 4 ||
        source[0] != 0x10) {
        return false;
    }

    const size_t declared_size = static_cast<size_t>(source[1]) |
        (static_cast<size_t>(source[2]) << 8) |
        (static_cast<size_t>(source[3]) << 16);

    if (declared_size != expected_size) {
        return false;
    }

    swiDecompressLZSSWram(source, destination_pointer);
    return true;
}


bool load_compressed_asset(
    const char* path,
    void* destination,
    size_t expected_size
) {
    size_t compressed_size = 0;
    if (!read_compressed_asset_file(path, &compressed_size)) {
        return false;
    }

    if (compressed_size >= ASSET_MAGIC_SIZE + 4 &&
        memcmp(
            compressed_asset_buffer,
            ASSET_MAGIC_RGB555,
            ASSET_MAGIC_SIZE
        ) == 0) {
        return decompress_lz10_asset(
            compressed_asset_buffer + ASSET_MAGIC_SIZE,
            compressed_size - ASSET_MAGIC_SIZE,
            destination,
            expected_size
        );
    }

    if (expected_size % sizeof(u16) != 0 ||
        compressed_size < ASSET_MAGIC_SIZE + ASSET_PALETTE_BYTES + 4 ||
        memcmp(
            compressed_asset_buffer,
            ASSET_MAGIC_PALETTE8,
            ASSET_MAGIC_SIZE
        ) != 0) {
        return false;
    }

    const size_t pixel_count = expected_size / sizeof(u16);
    const u8* palette = compressed_asset_buffer + ASSET_MAGIC_SIZE;
    const u8* index_stream = palette + ASSET_PALETTE_BYTES;
    const size_t index_stream_size = compressed_size -
        ASSET_MAGIC_SIZE - ASSET_PALETTE_BYTES;
    if (!decompress_lz10_asset(
            index_stream,
            index_stream_size,
            destination,
            pixel_count
        )) {
        return false;
    }

    // The decompressed indices occupy the first half of the u16 destination.
    // Expanding backwards is overlap-safe: each write touches only indices that
    // have already been read.
    u8* indices = static_cast<u8*>(destination);
    u16* pixels = static_cast<u16*>(destination);
    for (size_t index = pixel_count; index > 0; --index) {
        const u8 palette_index = indices[index - 1];
        const size_t palette_offset = static_cast<size_t>(palette_index) * 2;
        pixels[index - 1] = static_cast<u16>(
            palette[palette_offset] |
            (static_cast<u16>(palette[palette_offset + 1]) << 8) |
            BIT(15)
        );
    }
    return true;
}

bool format_page_path(
    char* path,
    size_t capacity,
    int chapter_index,
    int page_number,
    const char* asset_name
) {
    const int path_length = snprintf(
        path,
        capacity,
        "%s/%04d/page%03d_%s.dsm",
        current_book_path,
        chapters[chapter_index].number,
        page_number,
        asset_name
    );
    return path_length >= 0 && path_length < static_cast<int>(capacity);
}

bool load_full_page_asset(int buffer_index, int chapter_index, int page_number) {
    char path[PATH_CAPACITY];
    if (!format_page_path(
            path,
            sizeof(path),
            chapter_index,
            page_number,
            "full"
        )) {
        return false;
    }
    return load_compressed_asset(path, page_buffers[buffer_index], PAGE_BYTES);
}

bool load_preprocessed_zoom_asset(
    int chapter_index,
    int page_number,
    const char* asset_name,
    void* destination,
    size_t expected_bytes
) {
    char path[PATH_CAPACITY];
    if (!format_page_path(
            path,
            sizeof(path),
            chapter_index,
            page_number,
            asset_name
        )) {
        return false;
    }
    return load_compressed_asset(path, destination, expected_bytes);
}

bool uses_preprocessed_zoom_asset(int level) {
    return level == PREPROCESSED_ZOOM480_LEVEL ||
        level == PREPROCESSED_ZOOM512_LEVEL;
}

bool preprocessed_zoom_asset_info(
    int level,
    const char** asset_name,
    int* width,
    int* height,
    size_t* expected_bytes
) {
    if (asset_name == nullptr || width == nullptr || height == nullptr ||
        expected_bytes == nullptr) {
        return false;
    }
    if (level == PREPROCESSED_ZOOM480_LEVEL) {
        *asset_name = "zoom480";
        *width = PREPROCESSED_ZOOM480_WIDTH;
        *height = PREPROCESSED_ZOOM480_HEIGHT;
        *expected_bytes = PREPROCESSED_ZOOM480_BYTES;
        return true;
    }
    if (level == PREPROCESSED_ZOOM512_LEVEL) {
        *asset_name = "zoom512";
        *width = PREPROCESSED_ZOOM512_WIDTH;
        *height = PREPROCESSED_ZOOM512_HEIGHT;
        *expected_bytes = PREPROCESSED_ZOOM512_BYTES;
        return true;
    }
    return false;
}

bool uses_filtered_zoom_texture(int level) {
    return level >= FIRST_FILTERED_ZOOM && level <= LAST_FILTERED_ZOOM;
}

ImageSize filtered_zoom_texture_size(int level) {
    const CropSize crop = ZOOM_CROPS[level];
    return ImageSize{
        PAGE_WIDTH * SCREEN_WIDTH / crop.width,
        PAGE_HEIGHT * LOGICAL_HEIGHT / crop.height,
    };
}

inline void rotated_row_layout(
    int source_width,
    int source_height,
    int source_y,
    int rotation,
    int* destination_index,
    int* destination_step
) {
    switch (rotation & 3) {
        case 1: // 90 degrees clockwise: rotated size is height x width.
            *destination_index = source_height - 1 - source_y;
            *destination_step = source_height;
            break;
        case 2:
            *destination_index =
                (source_height - 1 - source_y) * source_width +
                source_width - 1;
            *destination_step = -1;
            break;
        case 3: // 90 degrees counterclockwise: rotated size is height x width.
            *destination_index =
                (source_width - 1) * source_height + source_y;
            *destination_step = -source_height;
            break;
        default:
            *destination_index = source_y * source_width;
            *destination_step = 1;
            break;
    }
}

bool prepare_rotated_preview_texture(int buffer_index, int rotation) {
    rotation &= 3;
    if (buffer_index < 0 || buffer_index > 1) {
        return false;
    }
    if (rotation == 0) {
        return true;
    }
    if (rotated_preview_page_buffer == buffer_index &&
        rotated_preview_rotation == rotation) {
        return true;
    }

    const u16* source = preview_buffers[buffer_index];
    for (int y = 0; y < PREVIEW_HEIGHT; ++y) {
        int destination_index = 0;
        int destination_step = 0;
        rotated_row_layout(
            PREVIEW_WIDTH,
            PREVIEW_HEIGHT,
            y,
            rotation,
            &destination_index,
            &destination_step
        );
        for (int x = 0; x < PREVIEW_WIDTH; ++x) {
            rotated_preview_buffer[destination_index] =
                source[y * PREVIEW_WIDTH + x];
            destination_index += destination_step;
        }
    }

    rotated_preview_page_buffer = buffer_index;
    rotated_preview_rotation = rotation;
    return true;
}

bool build_area_filter_span(
    int destination_index,
    int source_extent,
    int destination_extent,
    AreaFilterSpan* span
) {
    // Pixel edges are represented in units of destination_extent. This keeps
    // the exact source-pixel coverage rational and avoids floating point on ARM9.
    const int interval_start = destination_index * source_extent;
    const int interval_end = (destination_index + 1) * source_extent;
    const int first = interval_start / destination_extent;
    const int last = (interval_end - 1) / destination_extent;
    const int count = last - first + 1;
    if (count < 1 || count > MAX_AREA_FILTER_TAPS) {
        return false;
    }

    span->first = first;
    span->count = count;
    int cumulative_overlap = 0;
    int assigned_weight = 0;

    for (int tap = 0; tap < count; ++tap) {
        const int source_index = first + tap;
        const int source_start = source_index * destination_extent;
        const int source_end = source_start + destination_extent;
        const int overlap_start = interval_start > source_start
            ? interval_start
            : source_start;
        const int overlap_end = interval_end < source_end
            ? interval_end
            : source_end;
        cumulative_overlap += overlap_end - overlap_start;

        // Cumulative rounding ensures that all tap weights add to exactly 256.
        const int cumulative_weight = tap + 1 == count
            ? 256
            : (cumulative_overlap * 256 + source_extent / 2) / source_extent;
        span->weights[tap] = static_cast<u16>(
            cumulative_weight - assigned_weight
        );
        assigned_weight = cumulative_weight;
    }
    for (int tap = count; tap < MAX_AREA_FILTER_TAPS; ++tap) {
        span->weights[tap] = 0;
    }
    return assigned_weight == 256;
}

void filter_zoom_texture_generic(
    const u16* __restrict__ source,
    u16* __restrict__ destination,
    const ImageSize& texture_size,
    int rotation
) {
    // Direct 2-D filter. On ARM946E-S this is intentionally preferred for
    // fractional ratios: it keeps the working set tiny and reads source pixels
    // directly instead of writing/reading a large RGB intermediate row cache.
    for (int y = 0; y < texture_size.height; ++y) {
        const AreaFilterSpan& vertical_span = vertical_filter_spans[y];
        int destination_index = 0;
        int destination_step = 0;
        rotated_row_layout(
            texture_size.width,
            texture_size.height,
            y,
            rotation,
            &destination_index,
            &destination_step
        );

        for (int x = 0; x < texture_size.width; ++x) {
            const AreaFilterSpan& horizontal_span = horizontal_filter_spans[x];
            u32 red_sum = 0;
            u32 green_sum = 0;
            u32 blue_sum = 0;

            for (int y_tap = 0; y_tap < vertical_span.count; ++y_tap) {
                const u16* source_row = source +
                    (vertical_span.first + y_tap) * PAGE_WIDTH +
                    horizontal_span.first;
                const u32 vertical_weight = vertical_span.weights[y_tap];

                // MAX_AREA_FILTER_TAPS is only four. Keep the small loop local;
                // GCC -O3 can unroll/version it without creating an intermediate
                // image that thrashes the 4 KiB ARM9 data cache.
                for (int x_tap = 0; x_tap < horizontal_span.count; ++x_tap) {
                    const u16 pixel = source_row[x_tap];
                    const u32 weight = vertical_weight *
                        horizontal_span.weights[x_tap];
                    red_sum += (pixel & 31) * weight;
                    green_sum += ((pixel >> 5) & 31) * weight;
                    blue_sum += ((pixel >> 10) & 31) * weight;
                }
            }

            destination[destination_index] = static_cast<u16>(
                BIT(15) |
                ((red_sum + 32768) >> 16) |
                (((green_sum + 32768) >> 16) << 5) |
                (((blue_sum + 32768) >> 16) << 10)
            );
            destination_index += destination_step;
        }
    }
}

void filter_zoom_texture_2x(
    const u16* __restrict__ source,
    u16* __restrict__ destination,
    const ImageSize& texture_size,
    int rotation
) {
    for (int y = 0; y < texture_size.height; ++y) {
        const u16* row0 = source + (y * 2) * PAGE_WIDTH;
        const u16* row1 = row0 + PAGE_WIDTH;
        int destination_index = 0;
        int destination_step = 0;
        rotated_row_layout(
            texture_size.width,
            texture_size.height,
            y,
            rotation,
            &destination_index,
            &destination_step
        );

        for (int x = 0; x < texture_size.width; ++x) {
            const int source_x = x * 2;
            const u16 p00 = row0[source_x];
            const u16 p01 = row0[source_x + 1];
            const u16 p10 = row1[source_x];
            const u16 p11 = row1[source_x + 1];

            const u32 red =
                (p00 & 31) + (p01 & 31) + (p10 & 31) + (p11 & 31);
            const u32 green =
                ((p00 >> 5) & 31) + ((p01 >> 5) & 31) +
                ((p10 >> 5) & 31) + ((p11 >> 5) & 31);
            const u32 blue =
                ((p00 >> 10) & 31) + ((p01 >> 10) & 31) +
                ((p10 >> 10) & 31) + ((p11 >> 10) & 31);

            destination[destination_index] = static_cast<u16>(
                BIT(15) |
                ((red + 2) >> 2) |
                (((green + 2) >> 2) << 5) |
                (((blue + 2) >> 2) << 10)
            );
            destination_index += destination_step;
        }
    }
}

inline u32 weighted_three_85_86_85(u32 a, u32 b, u32 c) {
    // 85*a + 86*b + 85*c == 85*(a+b+c) + b.
    return 85 * (a + b + c) + b;
}

void filter_zoom_texture_3x(
    const u16* __restrict__ source,
    u16* __restrict__ destination,
    const ImageSize& texture_size,
    int rotation
) {
    for (int y = 0; y < texture_size.height; ++y) {
        const u16* rows[3] = {
            source + (y * 3) * PAGE_WIDTH,
            source + (y * 3 + 1) * PAGE_WIDTH,
            source + (y * 3 + 2) * PAGE_WIDTH,
        };
        int destination_index = 0;
        int destination_step = 0;
        rotated_row_layout(
            texture_size.width,
            texture_size.height,
            y,
            rotation,
            &destination_index,
            &destination_step
        );

        for (int x = 0; x < texture_size.width; ++x) {
            const int source_x = x * 3;
            u32 horizontal_red[3];
            u32 horizontal_green[3];
            u32 horizontal_blue[3];

            for (int row = 0; row < 3; ++row) {
                const u16 p0 = rows[row][source_x];
                const u16 p1 = rows[row][source_x + 1];
                const u16 p2 = rows[row][source_x + 2];
                horizontal_red[row] = weighted_three_85_86_85(
                    p0 & 31,
                    p1 & 31,
                    p2 & 31
                );
                horizontal_green[row] = weighted_three_85_86_85(
                    (p0 >> 5) & 31,
                    (p1 >> 5) & 31,
                    (p2 >> 5) & 31
                );
                horizontal_blue[row] = weighted_three_85_86_85(
                    (p0 >> 10) & 31,
                    (p1 >> 10) & 31,
                    (p2 >> 10) & 31
                );
            }

            const u32 red_sum = weighted_three_85_86_85(
                horizontal_red[0], horizontal_red[1], horizontal_red[2]
            );
            const u32 green_sum = weighted_three_85_86_85(
                horizontal_green[0], horizontal_green[1], horizontal_green[2]
            );
            const u32 blue_sum = weighted_three_85_86_85(
                horizontal_blue[0], horizontal_blue[1], horizontal_blue[2]
            );

            destination[destination_index] = static_cast<u16>(
                BIT(15) |
                ((red_sum + 32768) >> 16) |
                (((green_sum + 32768) >> 16) << 5) |
                (((blue_sum + 32768) >> 16) << 10)
            );
            destination_index += destination_step;
        }
    }
}

bool prepare_filtered_zoom_texture(
    int buffer_index,
    int level,
    int rotation,
    int chapter_index,
    int page_number
) {
    rotation &= 3;
    if (!uses_filtered_zoom_texture(level) ||
        buffer_index < 0 || buffer_index > 1) {
        return false;
    }
    if (zoom_texture_page_buffer == buffer_index &&
        zoom_texture_level == level &&
        zoom_texture_rotation == rotation) {
        return true;
    }

    const ImageSize texture_size = filtered_zoom_texture_size(level);
    if (texture_size.width < 1 || texture_size.width > MAX_ZOOM_TEXTURE_WIDTH ||
        texture_size.height < 1 || texture_size.height > MAX_ZOOM_TEXTURE_HEIGHT ||
        PAGE_WIDTH * SCREEN_WIDTH % ZOOM_CROPS[level].width != 0 ||
        PAGE_HEIGHT * LOGICAL_HEIGHT % ZOOM_CROPS[level].height != 0) {
        return false;
    }

    // Levels 3 and 4 map to exact 480x720 and 512x768 caches. Prefer the
    // PC-generated asset so the ARM9 only reads and BIOS-decompresses it.
    // Missing assets from older books transparently fall back to the original
    // full-page filter below.
    const char* preprocessed_asset_name = nullptr;
    int preprocessed_width = 0;
    int preprocessed_height = 0;
    size_t preprocessed_bytes = 0;
    const bool has_preprocessed_asset = preprocessed_zoom_asset_info(
        level,
        &preprocessed_asset_name,
        &preprocessed_width,
        &preprocessed_height,
        &preprocessed_bytes
    ) && texture_size.width == preprocessed_width &&
        texture_size.height == preprocessed_height;

    if (has_preprocessed_asset) {
        bool loaded = false;
        if (rotation == 0) {
            loaded = load_preprocessed_zoom_asset(
                chapter_index,
                page_number,
                preprocessed_asset_name,
                zoom_texture_buffer,
                preprocessed_bytes
            );
        } else {
            loaded = load_preprocessed_zoom_asset(
                chapter_index,
                page_number,
                preprocessed_asset_name,
                preprocessed_zoom_buffer,
                preprocessed_bytes
            );
            if (loaded) {
                for (int y = 0; y < preprocessed_height; ++y) {
                    int destination_index = 0;
                    int destination_step = 0;
                    rotated_row_layout(
                        preprocessed_width,
                        preprocessed_height,
                        y,
                        rotation,
                        &destination_index,
                        &destination_step
                    );
                    const u16* source_row = preprocessed_zoom_buffer +
                        y * preprocessed_width;
                    for (int x = 0; x < preprocessed_width; ++x) {
                        zoom_texture_buffer[destination_index] = source_row[x];
                        destination_index += destination_step;
                    }
                }
            }
        }

        if (loaded) {
            zoom_texture_page_buffer = buffer_index;
            zoom_texture_level = level;
            zoom_texture_rotation = rotation;
            return true;
        }

        // Compatibility path for books exported before this zoom asset existed.
        if (!full_page_loaded[buffer_index]) {
            if (!load_full_page_asset(
                    buffer_index,
                    chapter_index,
                    page_number
                )) {
                return false;
            }
            full_page_loaded[buffer_index] = true;
        }
    } else if (!full_page_loaded[buffer_index]) {
        return false;
    }

    // Exact 2x and 3x reductions use compact dedicated kernels. Fractional
    // ratios deliberately use the direct 2-D filter: on the DSi ARM9 its much
    // smaller working set beats the separable version, which created heavy
    // intermediate-memory traffic against a 4 KiB data cache.
    if (level != FILTERED_ZOOM_2X_LEVEL && level != FILTERED_ZOOM_3X_LEVEL) {
        for (int x = 0; x < texture_size.width; ++x) {
            if (!build_area_filter_span(
                    x,
                    PAGE_WIDTH,
                    texture_size.width,
                    &horizontal_filter_spans[x]
                )) {
                return false;
            }
        }
        for (int y = 0; y < texture_size.height; ++y) {
            if (!build_area_filter_span(
                    y,
                    PAGE_HEIGHT,
                    texture_size.height,
                    &vertical_filter_spans[y]
                )) {
                return false;
            }
        }
    }

    // All checks that can fail are complete. From here the fixed dimensions
    // guarantee valid accesses, so replacing the single cache remains
    // transactional with respect to missing/corrupt page files.
    zoom_texture_level = -1;
    zoom_texture_page_buffer = -1;
    zoom_texture_rotation = -1;

    const u16* source = page_buffers[buffer_index];
    u16* destination = zoom_texture_buffer;
    if (level == FILTERED_ZOOM_2X_LEVEL) {
        filter_zoom_texture_2x(source, destination, texture_size, rotation);
    } else if (level == FILTERED_ZOOM_3X_LEVEL) {
        filter_zoom_texture_3x(source, destination, texture_size, rotation);
    } else {
        filter_zoom_texture_generic(source, destination, texture_size, rotation);
    }

    zoom_texture_page_buffer = buffer_index;
    zoom_texture_level = level;
    zoom_texture_rotation = rotation;
    return true;
}

bool load_page_asset(int chapter_index, int page_number) {
    if (chapter_index < 0 || chapter_index >= chapter_count ||
        page_number < 1 || page_number > chapters[chapter_index].page_count) {
        return false;
    }

    const int candidate_buffer = active_page_buffer ^ 1;
    full_page_loaded[candidate_buffer] = false;
    if (rotated_preview_page_buffer == candidate_buffer) {
        rotated_preview_page_buffer = -1;
        rotated_preview_rotation = -1;
    }
    if (zoom_texture_page_buffer == candidate_buffer) {
        zoom_texture_page_buffer = -1;
        zoom_texture_level = -1;
        zoom_texture_rotation = -1;
    }
    char path[PATH_CAPACITY];
    if (!format_page_path(
            path,
            sizeof(path),
            chapter_index,
            page_number,
            "preview"
        ) ||
        !load_compressed_asset(
            path,
            preview_buffers[candidate_buffer],
            PREVIEW_BYTES
        )) {
        return false;
    }

    if (zoom_index > 0) {
        // At either preprocessed middle zoom, defer loading the much larger
        // 960x1440 page unless that page's host-generated asset is absent.
        if (!uses_preprocessed_zoom_asset(zoom_index)) {
            if (!load_full_page_asset(candidate_buffer, chapter_index, page_number)) {
                return false;
            }
            full_page_loaded[candidate_buffer] = true;
        }
        if (uses_filtered_zoom_texture(zoom_index) &&
            !prepare_filtered_zoom_texture(
                candidate_buffer,
                zoom_index,
                rotation_quarters_clockwise,
                chapter_index,
                page_number
            )) {
            return false;
        }
    }

    active_page_buffer = candidate_buffer;
    return true;
}

bool ensure_full_page_loaded() {
    if (full_page_loaded[active_page_buffer]) {
        return true;
    }

    if (!load_full_page_asset(
            active_page_buffer,
            current_chapter_index,
            current_page
        )) {
        return false;
    }
    full_page_loaded[active_page_buffer] = true;
    return true;
}

int floor_divide(int value, int divisor) {
    if (value >= 0) {
        return value / divisor;
    }
    return -((-value + divisor - 1) / divisor);
}

ImageSize rotated_size(int width, int height, int rotation) {
    return (rotation & 1) != 0
        ? ImageSize{height, width}
        : ImageSize{width, height};
}

// Convert an integer pixel in the rotated image back to the stored, upright asset.
// The caller must first check the point against rotated_size().
inline void rotated_pixel_to_source(
    int rotated_x,
    int rotated_y,
    int source_width,
    int source_height,
    int rotation,
    int* source_x,
    int* source_y
) {
    switch (rotation) {
        case 1: // 90 degrees clockwise
            *source_x = rotated_y;
            *source_y = source_height - 1 - rotated_x;
            break;
        case 2: // 180 degrees
            *source_x = source_width - 1 - rotated_x;
            *source_y = source_height - 1 - rotated_y;
            break;
        case 3: // 90 degrees counterclockwise
            *source_x = source_width - 1 - rotated_y;
            *source_y = rotated_x;
            break;
        default:
            *source_x = rotated_x;
            *source_y = rotated_y;
            break;
    }
}

// View centers are represented at twice their real coordinate so a half-pixel
// center is not discarded before applying a quarter-turn transformation.
void rotated_edge_to_source_twice(
    int rotated_x_twice,
    int rotated_y_twice,
    int rotation,
    int* source_x_twice,
    int* source_y_twice
) {
    switch (rotation) {
        case 1:
            *source_x_twice = rotated_y_twice;
            *source_y_twice = 2 * PAGE_HEIGHT - rotated_x_twice;
            break;
        case 2:
            *source_x_twice = 2 * PAGE_WIDTH - rotated_x_twice;
            *source_y_twice = 2 * PAGE_HEIGHT - rotated_y_twice;
            break;
        case 3:
            *source_x_twice = 2 * PAGE_WIDTH - rotated_y_twice;
            *source_y_twice = rotated_x_twice;
            break;
        default:
            *source_x_twice = rotated_x_twice;
            *source_y_twice = rotated_y_twice;
            break;
    }
}

void source_edge_to_rotated_twice(
    int source_x_twice,
    int source_y_twice,
    int rotation,
    int* rotated_x_twice,
    int* rotated_y_twice
) {
    switch (rotation) {
        case 1:
            *rotated_x_twice = 2 * PAGE_HEIGHT - source_y_twice;
            *rotated_y_twice = source_x_twice;
            break;
        case 2:
            *rotated_x_twice = 2 * PAGE_WIDTH - source_x_twice;
            *rotated_y_twice = 2 * PAGE_HEIGHT - source_y_twice;
            break;
        case 3:
            *rotated_x_twice = source_y_twice;
            *rotated_y_twice = 2 * PAGE_WIDTH - source_x_twice;
            break;
        default:
            *rotated_x_twice = source_x_twice;
            *rotated_y_twice = source_y_twice;
            break;
    }
}

void clamp_view() {
    const CropSize crop = ZOOM_CROPS[zoom_index];
    const ImageSize page = rotated_size(
        PAGE_WIDTH,
        PAGE_HEIGHT,
        rotation_quarters_clockwise
    );
    // The virtual viewport may move one complete viewport beyond any page edge.
    // At the limit, the page touches the opposite screen boundary geometrically,
    // while all sampled LCD pixels may be the white background.
    const int min_x = -crop.width;
    const int min_y = -crop.height;
    const int max_x = page.width;
    const int max_y = page.height;

    if (view_x < min_x) view_x = min_x;
    if (view_y < min_y) view_y = min_y;
    if (view_x > max_x) view_x = max_x;
    if (view_y > max_y) view_y = max_y;
}

void center_view() {
    const CropSize crop = ZOOM_CROPS[zoom_index];
    const ImageSize page = rotated_size(
        PAGE_WIDTH,
        PAGE_HEIGHT,
        rotation_quarters_clockwise
    );
    view_x = floor_divide(page.width - crop.width, 2);
    view_y = floor_divide(page.height - crop.height, 2);
    clamp_view();
}

bool reset_zoom_and_center_view() {
    const int previous_zoom = zoom_index;
    const int previous_view_x = view_x;
    const int previous_view_y = view_y;

    zoom_index = 0;
    center_view();

    const bool zoom_changed = zoom_index != previous_zoom;
    if (zoom_changed) {
        persist_current_state();
    }
    return zoom_changed ||
        view_x != previous_view_x ||
        view_y != previous_view_y;
}

bool select_page(int chapter_index, int page_number, bool save_state = true) {
    if (!load_page_asset(chapter_index, page_number)) {
        return false;
    }

    current_chapter_index = chapter_index;
    current_page = page_number;
    // A newly selected page keeps the current zoom but starts centered.
    center_view();
    if (save_state) {
        // Navigation remains usable if the SD card becomes read-only; the page
        // load succeeds even when its resume marker cannot be saved.
        persist_current_state();
    }
    return true;
}

bool load_saved_or_first_page() {
    const int resume_chapter_index = current_chapter_index;
    const int resume_page = current_page;
    if (select_page(resume_chapter_index, resume_page, false)) {
        return true;
    }

    if (resume_chapter_index == 0 && resume_page == 1) {
        return false;
    }
    if (!select_page(0, 1, false)) {
        return false;
    }

    // Repair a stale marker when a previously saved asset was removed.
    persist_current_state();
    return true;
}

bool switch_book_transactionally(int next_book_index) {
    if (next_book_index < 0 || next_book_index >= book_count) {
        return false;
    }
    if (next_book_index == current_book_index) {
        return true;
    }

    memcpy(manifest_scratch, chapters, sizeof(chapters));
    char previous_book_path[PATH_CAPACITY];
    strcpy(previous_book_path, current_book_path);
    const int previous_book_index = current_book_index;
    const int previous_chapter_count = chapter_count;
    const int previous_chapter_index = current_chapter_index;
    const int previous_page = current_page;
    const int previous_active_page_buffer = active_page_buffer;
    const bool previous_full_page_loaded_0 = full_page_loaded[0];
    const bool previous_full_page_loaded_1 = full_page_loaded[1];
    const int previous_zoom_index = zoom_index;
    const int previous_view_x = view_x;
    const int previous_view_y = view_y;
    const int previous_rotation = rotation_quarters_clockwise;

    bool switched = activate_book(next_book_index);
    if (switched) {
        // activate_book restored the saved page, rotation, and stable crop.
        // The transactional page load prepares the matching rotated cache.
        switched = load_saved_or_first_page();
    }

    if (switched) {
        return true;
    }

    memcpy(chapters, manifest_scratch, sizeof(chapters));
    strcpy(current_book_path, previous_book_path);
    current_book_index = previous_book_index;
    chapter_count = previous_chapter_count;
    current_chapter_index = previous_chapter_index;
    current_page = previous_page;
    active_page_buffer = previous_active_page_buffer;
    full_page_loaded[0] = previous_full_page_loaded_0;
    full_page_loaded[1] = previous_full_page_loaded_1;
    zoom_index = previous_zoom_index;
    view_x = previous_view_x;
    view_y = previous_view_y;
    rotation_quarters_clockwise = previous_rotation;
    return false;
}

bool select_adjacent_page(int direction) {
    int chapter_index = current_chapter_index;
    int page_number = current_page + direction;

    if (direction > 0 &&
        page_number > chapters[chapter_index].page_count) {
        if (chapter_index + 1 >= chapter_count) {
            return false;
        }
        ++chapter_index;
        page_number = 1;
    } else if (direction < 0 && page_number < 1) {
        if (chapter_index == 0) {
            return false;
        }
        --chapter_index;
        page_number = chapters[chapter_index].page_count;
    }

    return select_page(chapter_index, page_number);
}

void change_zoom(int direction) {
    const int next_zoom = zoom_index + direction;
    if (next_zoom < 0 || next_zoom >= ZOOM_LEVEL_COUNT || next_zoom == zoom_index) {
        return;
    }

    const CropSize old_crop = ZOOM_CROPS[zoom_index];
    const int center_x_twice = view_x * 2 + old_crop.width;
    const int center_y_twice = view_y * 2 + old_crop.height;

    if (uses_filtered_zoom_texture(next_zoom)) {
        // Books exported by the new pipeline can enter levels 3 and 4 without
        // loading the 960x1440 detail page. Other levels still require it.
        if (!uses_preprocessed_zoom_asset(next_zoom) &&
            !full_page_loaded[active_page_buffer] &&
            !ensure_full_page_loaded()) {
            return;
        }
        // The texture is built or loaded only when the zoom changes. Panning
        // can then render it at full speed instead of filtering every frame.
        if (!prepare_filtered_zoom_texture(
            active_page_buffer,
            next_zoom,
            rotation_quarters_clockwise,
            current_chapter_index,
            current_page
        )) {
            return;
        }
    }
    zoom_index = next_zoom;
    const CropSize new_crop = ZOOM_CROPS[zoom_index];
    view_x = floor_divide(center_x_twice - new_crop.width, 2);
    view_y = floor_divide(center_y_twice - new_crop.height, 2);
    clamp_view();
    persist_current_state();
}

bool rotate_view(int quarter_turns_clockwise) {
    const CropSize crop = ZOOM_CROPS[zoom_index];
    const int previous_rotation = rotation_quarters_clockwise;
    const int previous_view_x = view_x;
    const int previous_view_y = view_y;
    const int old_center_x_twice = view_x * 2 + crop.width;
    const int old_center_y_twice = view_y * 2 + crop.height;

    int source_center_x_twice = 0;
    int source_center_y_twice = 0;
    rotated_edge_to_source_twice(
        old_center_x_twice,
        old_center_y_twice,
        rotation_quarters_clockwise,
        &source_center_x_twice,
        &source_center_y_twice
    );

    rotation_quarters_clockwise =
        (rotation_quarters_clockwise + quarter_turns_clockwise + 4) & 3;

    int new_center_x_twice = 0;
    int new_center_y_twice = 0;
    source_edge_to_rotated_twice(
        source_center_x_twice,
        source_center_y_twice,
        rotation_quarters_clockwise,
        &new_center_x_twice,
        &new_center_y_twice
    );

    view_x = floor_divide(new_center_x_twice - crop.width, 2);
    view_y = floor_divide(new_center_y_twice - crop.height, 2);
    clamp_view();

    if (uses_filtered_zoom_texture(zoom_index) &&
        !prepare_filtered_zoom_texture(
            active_page_buffer,
            zoom_index,
            rotation_quarters_clockwise,
            current_chapter_index,
            current_page
        )) {
        rotation_quarters_clockwise = previous_rotation;
        view_x = previous_view_x;
        view_y = previous_view_y;
        return false;
    }
    persist_current_state();
    return true;
}

bool pan_view(u32 repeated_keys) {
    const CropSize crop = ZOOM_CROPS[zoom_index];
    const int horizontal_step = (crop.width * PAN_STEP_PIXELS + SCREEN_WIDTH - 1) / SCREEN_WIDTH;
    const int vertical_step = (crop.height * PAN_STEP_PIXELS + LOGICAL_HEIGHT - 1) / LOGICAL_HEIGHT;
    const int old_x = view_x;
    const int old_y = view_y;

    // The D-pad moves the viewport, so the page moves in the opposite direction.
    // view_x/view_y use rotated-image coordinates, which makes these axes rotate
    // with the displayed page after L or R is pressed.
    if (repeated_keys & KEY_LEFT) view_x -= horizontal_step;
    if (repeated_keys & KEY_RIGHT) view_x += horizontal_step;
    if (repeated_keys & KEY_UP) view_y -= vertical_step;
    if (repeated_keys & KEY_DOWN) view_y += vertical_step;

    clamp_view();
    return view_x != old_x || view_y != old_y;
}

int scale_drag_delta(int pixel_delta, int source_extent, int screen_extent) {
    const int magnitude = pixel_delta < 0 ? -pixel_delta : pixel_delta;
    const int scaled = (magnitude * source_extent + screen_extent / 2) / screen_extent;
    return pixel_delta < 0 ? -scaled : scaled;
}

bool drag_view(DragState* drag_state) {
    touchPosition touch = {};
    if (!touchRead(&touch)) {
        drag_state->active = false;
        return false;
    }

    if (!drag_state->active) {
        drag_state->active = true;
        drag_state->touch_x = touch.px;
        drag_state->touch_y = touch.py;
        drag_state->starting_view_x = view_x;
        drag_state->starting_view_y = view_y;
        return false;
    }

    const CropSize crop = ZOOM_CROPS[zoom_index];
    const int old_x = view_x;
    const int old_y = view_y;
    const int touch_delta_x = static_cast<int>(touch.px) - drag_state->touch_x;
    const int touch_delta_y = static_cast<int>(touch.py) - drag_state->touch_y;

    // Subtracting the finger displacement makes the page follow the finger.
    view_x = drag_state->starting_view_x -
        scale_drag_delta(touch_delta_x, crop.width, SCREEN_WIDTH);
    view_y = drag_state->starting_view_y -
        scale_drag_delta(touch_delta_y, crop.height, LOGICAL_HEIGHT);
    clamp_view();
    return view_x != old_x || view_y != old_y;
}

void copy_cached_texture_slice(
    u16* destination,
    int logical_y_start,
    const u16* source,
    int source_width,
    int source_height,
    int starting_x,
    int starting_y
) ITCM_CODE;

void copy_cached_texture_slice(
    u16* destination,
    int logical_y_start,
    const u16* source,
    int source_width,
    int source_height,
    int starting_x,
    int starting_y
) {
    const int first_source_y = starting_y + logical_y_start;

    // Normal panning spends almost all of its time fully inside the cached
    // texture.  Skip all clipping/white-fill branches in that common case.
    if (starting_x >= 0 && starting_x + SCREEN_WIDTH <= source_width &&
        first_source_y >= 0 && first_source_y + SCREEN_HEIGHT <= source_height) {
        const u16* source_row =
            source + first_source_y * source_width + starting_x;
        u16* destination_row = destination;
        for (int screen_y = 0; screen_y < SCREEN_HEIGHT; ++screen_y) {
            memcpy(destination_row, source_row, SCREEN_WIDTH * sizeof(u16));
            destination_row += SCREEN_WIDTH;
            source_row += source_width;
        }
        return;
    }

    for (int screen_y = 0; screen_y < SCREEN_HEIGHT; ++screen_y) {
        u16* destination_row = &destination[screen_y * SCREEN_WIDTH];
        const int source_y = starting_y + logical_y_start + screen_y;
        if (source_y < 0 || source_y >= source_height) {
            memset(destination_row, 0xff, SCREEN_WIDTH * sizeof(u16));
            continue;
        }

        int visible_start = starting_x < 0 ? -starting_x : 0;
        int visible_end = SCREEN_WIDTH;
        if (starting_x + visible_end > source_width) {
            visible_end = source_width - starting_x;
        }
        if (visible_start < 0) visible_start = 0;
        if (visible_start > SCREEN_WIDTH) visible_start = SCREEN_WIDTH;
        if (visible_end < 0) visible_end = 0;
        if (visible_end > SCREEN_WIDTH) visible_end = SCREEN_WIDTH;
        if (visible_start >= visible_end) {
            memset(destination_row, 0xff, SCREEN_WIDTH * sizeof(u16));
            continue;
        }

        if (visible_start != 0 || visible_end != SCREEN_WIDTH) {
            memset(destination_row, 0xff, SCREEN_WIDTH * sizeof(u16));
        }

        memcpy(
            destination_row + visible_start,
            &source[
                source_y * source_width + starting_x + visible_start
            ],
            static_cast<size_t>(visible_end - visible_start) * sizeof(u16)
        );
    }
}

void render_preview_slice(u16* destination, int logical_y_start) {
    const int rotation = rotation_quarters_clockwise & 3;
    const u16* source = preview_buffers[active_page_buffer];
    if (rotation != 0) {
        if (!prepare_rotated_preview_texture(active_page_buffer, rotation)) {
            memset(destination, 0xff, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(u16));
            return;
        }
        source = rotated_preview_buffer;
    }

    const ImageSize rotated_page = rotated_size(
        PAGE_WIDTH,
        PAGE_HEIGHT,
        rotation
    );
    const ImageSize rotated_preview = rotated_size(
        PREVIEW_WIDTH,
        PREVIEW_HEIGHT,
        rotation
    );
    const int preview_view_x = floor_divide(
        view_x * rotated_preview.width,
        rotated_page.width
    );
    const int preview_view_y = floor_divide(
        view_y * rotated_preview.height,
        rotated_page.height
    );
    copy_cached_texture_slice(
        destination,
        logical_y_start,
        source,
        rotated_preview.width,
        rotated_preview.height,
        preview_view_x,
        preview_view_y
    );
}

RotatedTextureLayout make_rotated_texture_layout(
    int source_width,
    int source_height,
    int rotation
) {
    switch (rotation & 3) {
        case 1: // 90 degrees clockwise
            return RotatedTextureLayout{
                source_height,
                source_width,
                (source_height - 1) * source_width,
                -source_width,
                1,
            };
        case 2: // 180 degrees
            return RotatedTextureLayout{
                source_width,
                source_height,
                source_width * source_height - 1,
                -1,
                -source_width,
            };
        case 3: // 90 degrees counterclockwise
            return RotatedTextureLayout{
                source_height,
                source_width,
                source_width - 1,
                source_width,
                -1,
            };
        default:
            return RotatedTextureLayout{
                source_width,
                source_height,
                0,
                1,
                source_width,
            };
    }
}

inline u16 bilinear_rgb555_fast(
    u16 top_left,
    u16 top_right,
    u16 bottom_left,
    u16 bottom_right,
    u32 fraction_x,
    u32 fraction_y
) {
    // Do the interpolation as two exact 1-D lerps without rounding the
    // horizontal intermediate.  This is algebraically identical to the old
    // four 2-D weights but needs only three multiplies per color channel.
    const int r00 = top_left & 31;
    const int r10 = top_right & 31;
    const int r01 = bottom_left & 31;
    const int r11 = bottom_right & 31;
    const int g00 = (top_left >> 5) & 31;
    const int g10 = (top_right >> 5) & 31;
    const int g01 = (bottom_left >> 5) & 31;
    const int g11 = (bottom_right >> 5) & 31;
    const int b00 = (top_left >> 10) & 31;
    const int b10 = (top_right >> 10) & 31;
    const int b01 = (bottom_left >> 10) & 31;
    const int b11 = (bottom_right >> 10) & 31;

    const int red_top = (r00 << 8) + (r10 - r00) * static_cast<int>(fraction_x);
    const int red_bottom = (r01 << 8) + (r11 - r01) * static_cast<int>(fraction_x);
    const int green_top = (g00 << 8) + (g10 - g00) * static_cast<int>(fraction_x);
    const int green_bottom = (g01 << 8) + (g11 - g01) * static_cast<int>(fraction_x);
    const int blue_top = (b00 << 8) + (b10 - b00) * static_cast<int>(fraction_x);
    const int blue_bottom = (b01 << 8) + (b11 - b01) * static_cast<int>(fraction_x);

    const int red = (
        (red_top << 8) +
        (red_bottom - red_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;
    const int green = (
        (green_top << 8) +
        (green_bottom - green_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;
    const int blue = (
        (blue_top << 8) +
        (blue_bottom - blue_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;

    return static_cast<u16>(BIT(15) | red | (green << 5) | (blue << 10));
}

// Scale a page-space coordinate into 16.16 texture space with floor semantics.
// C++ division truncates negative values toward zero, which would otherwise
// make the white overscroll boundary asymmetric.
s32 scale_coordinate_to_fixed(
    int coordinate,
    int texture_extent,
    int page_extent
) {
    const long long numerator =
        static_cast<long long>(coordinate) * texture_extent * (1 << 16);
    if (numerator >= 0) {
        return static_cast<s32>(numerator / page_extent);
    }
    return static_cast<s32>(
        -((-numerator + page_extent - 1) / page_extent)
    );
}

void build_bilinear_axis_table(
    s32 starting_fixed,
    s32 step_fixed,
    int count,
    int source_stride,
    s16* __restrict__ coordinates,
    s32* __restrict__ offsets,
    u8* __restrict__ fractions
) {
    s32 fixed = starting_fixed;
    for (int index = 0; index < count; ++index) {
        const int coordinate = floor_divide(fixed, 1 << 16);
        coordinates[index] = static_cast<s16>(coordinate);
        offsets[index] = coordinate * source_stride;
        fractions[index] = static_cast<u8>(
            static_cast<u32>(fixed - coordinate * (1 << 16)) >> 8
        );
        fixed += step_fixed;
    }
}

inline u16 sample_layout_edge_pixel(
    const u16* __restrict__ source,
    const RotatedTextureLayout& layout,
    int rotated_x,
    int rotated_y,
    s32 x_offset,
    s32 y_offset
) {
    if (rotated_x < 0 || rotated_x >= layout.width ||
        rotated_y < 0 || rotated_y >= layout.height) {
        return COLOR_WHITE;
    }
    return source[layout.origin + x_offset + y_offset];
}

inline u16 render_bilinear_edge_pixel(
    const u16* __restrict__ source,
    const RotatedTextureLayout& layout,
    int screen_x,
    int screen_y
) {
    const int rotated_x = bilinear_x_coordinates[screen_x];
    const int rotated_y = bilinear_y_coordinates[screen_y];
    const s32 x_offset = bilinear_x_offsets[screen_x];
    const s32 y_offset = bilinear_y_offsets[screen_y];

    const u16 top_left = sample_layout_edge_pixel(
        source, layout, rotated_x, rotated_y, x_offset, y_offset
    );
    const u16 top_right = sample_layout_edge_pixel(
        source,
        layout,
        rotated_x + 1,
        rotated_y,
        x_offset + layout.x_stride,
        y_offset
    );
    const u16 bottom_left = sample_layout_edge_pixel(
        source,
        layout,
        rotated_x,
        rotated_y + 1,
        x_offset,
        y_offset + layout.y_stride
    );
    const u16 bottom_right = sample_layout_edge_pixel(
        source,
        layout,
        rotated_x + 1,
        rotated_y + 1,
        x_offset + layout.x_stride,
        y_offset + layout.y_stride
    );

    return bilinear_rgb555_fast(
        top_left,
        top_right,
        bottom_left,
        bottom_right,
        bilinear_x_fractions[screen_x],
        bilinear_y_fractions[screen_y]
    );
}

void render_bilinear_texture_slice(
    u16* destination,
    int logical_y_start,
    const u16* source,
    int source_width,
    int source_height
) {
    const int rotation = rotation_quarters_clockwise & 3;
    const CropSize crop = ZOOM_CROPS[zoom_index];
    const ImageSize rotated_page = rotated_size(
        PAGE_WIDTH,
        PAGE_HEIGHT,
        rotation
    );
    const RotatedTextureLayout layout = make_rotated_texture_layout(
        source_width,
        source_height,
        rotation
    );
    const s32 x_step = static_cast<s32>(
        (static_cast<long long>(crop.width) * layout.width << 16) /
        (static_cast<long long>(SCREEN_WIDTH) * rotated_page.width)
    );
    const s32 y_step = static_cast<s32>(
        (static_cast<long long>(crop.height) * layout.height << 16) /
        (static_cast<long long>(LOGICAL_HEIGHT) * rotated_page.height)
    );

    const s32 starting_x =
        scale_coordinate_to_fixed(
            view_x,
            layout.width,
            rotated_page.width
        ) + x_step / 2 - (1 << 15);
    const s32 starting_y =
        scale_coordinate_to_fixed(
            view_y,
            layout.height,
            rotated_page.height
        ) + logical_y_start * y_step + y_step / 2 - (1 << 15);

    build_bilinear_axis_table(
        starting_x,
        x_step,
        SCREEN_WIDTH,
        layout.x_stride,
        bilinear_x_coordinates,
        bilinear_x_offsets,
        bilinear_x_fractions
    );
    build_bilinear_axis_table(
        starting_y,
        y_step,
        SCREEN_HEIGHT,
        layout.y_stride,
        bilinear_y_coordinates,
        bilinear_y_offsets,
        bilinear_y_fractions
    );

    // X coordinates are monotone because x_step is positive.  Split the LCD
    // row into white, one-pixel edge, and unchecked interior regions once, so
    // the hot interior loop has no bounds checks or rotation branches.
    int left_white_end = 0;
    while (left_white_end < SCREEN_WIDTH &&
           bilinear_x_coordinates[left_white_end] < -1) {
        ++left_white_end;
    }
    int interior_start = left_white_end;
    while (interior_start < SCREEN_WIDTH &&
           bilinear_x_coordinates[interior_start] < 0) {
        ++interior_start;
    }
    int interior_end = interior_start;
    while (interior_end < SCREEN_WIDTH &&
           bilinear_x_coordinates[interior_end] < layout.width - 1) {
        ++interior_end;
    }
    int right_white_start = interior_end;
    while (right_white_start < SCREEN_WIDTH &&
           bilinear_x_coordinates[right_white_start] < layout.width) {
        ++right_white_start;
    }

    for (int screen_y = 0; screen_y < SCREEN_HEIGHT; ++screen_y) {
        u16* destination_row = destination + screen_y * SCREEN_WIDTH;
        const int rotated_y = bilinear_y_coordinates[screen_y];

        // If both vertical taps are outside, every sample is white regardless
        // of X; avoid touching the source texture at all.
        if (rotated_y < -1 || rotated_y >= layout.height) {
            memset(destination_row, 0xff, SCREEN_WIDTH * sizeof(u16));
            continue;
        }

        if (left_white_end > 0) {
            memset(destination_row, 0xff, left_white_end * sizeof(u16));
        }
        if (right_white_start < SCREEN_WIDTH) {
            memset(
                destination_row + right_white_start,
                0xff,
                (SCREEN_WIDTH - right_white_start) * sizeof(u16)
            );
        }

        const bool vertical_interior =
            rotated_y >= 0 && rotated_y + 1 < layout.height;

        if (!vertical_interior) {
            // Only the top/bottom one-pixel blend band reaches this path.
            for (int x = left_white_end; x < right_white_start; ++x) {
                destination_row[x] = render_bilinear_edge_pixel(
                    source,
                    layout,
                    x,
                    screen_y
                );
            }
            continue;
        }

        // Left one-pixel blend band (rotated_x == -1), if present.
        for (int x = left_white_end; x < interior_start; ++x) {
            destination_row[x] = render_bilinear_edge_pixel(
                source,
                layout,
                x,
                screen_y
            );
        }

        const s32 y_offset = bilinear_y_offsets[screen_y];
        const u32 fraction_y = bilinear_y_fractions[screen_y];
        const int row_origin = layout.origin + y_offset;
        for (int x = interior_start; x < interior_end; ++x) {
            const int source_index = row_origin + bilinear_x_offsets[x];
            const u16 top_left = source[source_index];
            const u16 top_right = source[source_index + layout.x_stride];
            const u16 bottom_left = source[source_index + layout.y_stride];
            const u16 bottom_right = source[
                source_index + layout.x_stride + layout.y_stride
            ];
            destination_row[x] = bilinear_rgb555_fast(
                top_left,
                top_right,
                bottom_left,
                bottom_right,
                bilinear_x_fractions[x],
                fraction_y
            );
        }

        // Right one-pixel blend band (rotated_x == width - 1), if present.
        for (int x = interior_end; x < right_white_start; ++x) {
            destination_row[x] = render_bilinear_edge_pixel(
                source,
                layout,
                x,
                screen_y
            );
        }
    }
}

void render_bilinear_detail_slice(u16* destination, int logical_y_start) {
    render_bilinear_texture_slice(
        destination,
        logical_y_start,
        page_buffers[active_page_buffer],
        PAGE_WIDTH,
        PAGE_HEIGHT
    );
}

int nearest_fixed_coordinate(s32 value) {
    return floor_divide(value + (1 << 15), 1 << 16);
}

void render_cached_zoom_slice(u16* destination, int logical_y_start) ITCM_CODE;

void render_cached_zoom_slice(u16* destination, int logical_y_start) {
    if (zoom_texture_page_buffer != active_page_buffer ||
        zoom_texture_level != zoom_index ||
        zoom_texture_rotation != rotation_quarters_clockwise) {
        // This should only be reached after an unexpected cache-preparation
        // failure. Bilinear detail rendering is a safe, readable fallback.
        render_bilinear_detail_slice(destination, logical_y_start);
        return;
    }

    const ImageSize texture_size = filtered_zoom_texture_size(zoom_index);
    const ImageSize rotated_page = rotated_size(
        PAGE_WIDTH,
        PAGE_HEIGHT,
        rotation_quarters_clockwise
    );
    const ImageSize rotated_texture = rotated_size(
        texture_size.width,
        texture_size.height,
        rotation_quarters_clockwise
    );
    const int starting_x = nearest_fixed_coordinate(
        scale_coordinate_to_fixed(
            view_x,
            rotated_texture.width,
            rotated_page.width
        )
    );
    const int starting_y = nearest_fixed_coordinate(
        scale_coordinate_to_fixed(
            view_y,
            rotated_texture.height,
            rotated_page.height
        )
    );

    // The cache was physically stored in the active rotation, so every
    // orientation—including 90 degrees—uses the same contiguous row copies.
    copy_cached_texture_slice(
        destination,
        logical_y_start,
        zoom_texture_buffer,
        rotated_texture.width,
        rotated_texture.height,
        starting_x,
        starting_y
    );
}

void render_screen_slice(u16* destination, int logical_y_start) {
    if (zoom_index == 0) {
        render_preview_slice(destination, logical_y_start);
        return;
    }

    if (uses_filtered_zoom_texture(zoom_index)) {
        render_cached_zoom_slice(destination, logical_y_start);
        return;
    }

    // Defensive fallback for an invalid zoom index.
    render_bilinear_detail_slice(destination, logical_y_start);
}

void present_reader_view() {
    render_screen_slice(top_screen_buffer, 0);
    render_screen_slice(bottom_screen_buffer, SCREEN_HEIGHT);

    DC_FlushRange(top_screen_buffer, sizeof(top_screen_buffer));
    DC_FlushRange(bottom_screen_buffer, sizeof(bottom_screen_buffer));
    swiWaitForVBlank();
    dmaCopy(top_screen_buffer, bgGetGfxPtr(top_background), sizeof(top_screen_buffer));
    dmaCopy(bottom_screen_buffer, bgGetGfxPtr(bottom_background), sizeof(bottom_screen_buffer));
}

void fill_buffer(u16* buffer, u16 color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
        buffer[i] = color;
    }
}

void fill_rectangle(u16* buffer, int x, int y, int width, int height, u16 color) {
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > SCREEN_WIDTH) width = SCREEN_WIDTH - x;
    if (y + height > SCREEN_HEIGHT) height = SCREEN_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    for (int row = 0; row < height; ++row) {
        u16* destination = &buffer[(y + row) * SCREEN_WIDTH + x];
        for (int column = 0; column < width; ++column) {
            destination[column] = color;
        }
    }
}

void draw_rectangle(u16* buffer, int x, int y, int width, int height, u16 color) {
    fill_rectangle(buffer, x, y, width, 2, color);
    fill_rectangle(buffer, x, y + height - 2, width, 2, color);
    fill_rectangle(buffer, x, y, 2, height, color);
    fill_rectangle(buffer, x + width - 2, y, 2, height, color);
}

constexpr u8 DIGIT_FONT[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E},
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
};

void draw_digit(u16* buffer, int x, int y, int digit, int scale, u16 color) {
    if (digit < 0 || digit > 9 || scale < 1) return;

    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (DIGIT_FONT[digit][row] & (1U << (4 - column))) {
                fill_rectangle(
                    buffer,
                    x + column * scale,
                    y + row * scale,
                    scale,
                    scale,
                    color
                );
            }
        }
    }
}

int decimal_length(int value) {
    int length = 1;
    while (value >= 10) {
        value /= 10;
        ++length;
    }
    return length;
}

void draw_number(u16* buffer, int x, int y, int value, int scale, u16 color) {
    if (value < 0) return;
    const int length = decimal_length(value);
    const int advance = 6 * scale;
    int divisor = 1;
    for (int i = 1; i < length; ++i) divisor *= 10;

    for (int i = 0; i < length; ++i) {
        draw_digit(buffer, x + i * advance, y, (value / divisor) % 10, scale, color);
        divisor /= 10;
    }
}

void draw_backspace_icon(u16* buffer, int center_x, int center_y, u16 color) {
    fill_rectangle(buffer, center_x - 9, center_y - 1, 17, 3, color);
    for (int offset = 0; offset < 6; ++offset) {
        fill_rectangle(buffer, center_x - 9 + offset, center_y - 1 - offset, 2, 2, color);
        fill_rectangle(buffer, center_x - 9 + offset, center_y + 1 + offset, 2, 2, color);
    }
}

void draw_confirm_icon(u16* buffer, int center_x, int center_y, u16 color) {
    for (int offset = 0; offset < 6; ++offset) {
        fill_rectangle(buffer, center_x - 10 + offset, center_y + offset, 3, 3, color);
    }
    for (int offset = 0; offset < 10; ++offset) {
        fill_rectangle(buffer, center_x - 4 + offset, center_y + 5 - offset, 3, 3, color);
    }
}

void present_bottom_dialog() {
    DC_FlushRange(bottom_screen_buffer, sizeof(bottom_screen_buffer));
    swiWaitForVBlank();
    dmaCopy(bottom_screen_buffer, bgGetGfxPtr(bottom_background), sizeof(bottom_screen_buffer));
}

void draw_page_dialog(const char* input, bool invalid) {
    fill_buffer(bottom_screen_buffer, COLOR_BLACK);
    fill_rectangle(bottom_screen_buffer, 18, 5, 220, 182, COLOR_PANEL);
    draw_rectangle(
        bottom_screen_buffer,
        18,
        5,
        220,
        182,
        invalid ? COLOR_ERROR : COLOR_ACCENT
    );

    // The header shows the valid range as "1 - page_count".
    draw_number(bottom_screen_buffer, 30, 15, 1, 1, COLOR_WHITE);
    fill_rectangle(bottom_screen_buffer, 39, 18, 8, 2, COLOR_WHITE);
    draw_number(
        bottom_screen_buffer,
        51,
        15,
        chapters[current_chapter_index].page_count,
        1,
        COLOR_WHITE
    );

    const int input_length = static_cast<int>(strlen(input));
    if (input_length == 0) {
        fill_rectangle(bottom_screen_buffer, 112, 31, 32, 2, COLOR_WHITE);
    } else {
        const int scale = 2;
        const int total_width = input_length * 6 * scale - scale;
        const int start_x = (SCREEN_WIDTH - total_width) / 2;
        for (int i = 0; i < input_length; ++i) {
            draw_digit(bottom_screen_buffer, start_x + i * 6 * scale, 25, input[i] - '0', scale, COLOR_WHITE);
        }
    }

    constexpr int keypad_x = 32;
    constexpr int keypad_y = 54;
    constexpr int button_width = 60;
    constexpr int button_height = 28;
    constexpr int gap_x = 6;
    constexpr int gap_y = 4;

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 3; ++column) {
            const int x = keypad_x + column * (button_width + gap_x);
            const int y = keypad_y + row * (button_height + gap_y);
            const bool confirm_button = row == 3 && column == 2;
            fill_rectangle(
                bottom_screen_buffer,
                x,
                y,
                button_width,
                button_height,
                confirm_button ? COLOR_ACCENT : COLOR_BUTTON
            );
            draw_rectangle(bottom_screen_buffer, x, y, button_width, button_height, COLOR_WHITE);

            if (row < 3) {
                const int digit = row * 3 + column + 1;
                draw_digit(bottom_screen_buffer, x + 25, y + 7, digit, 2, COLOR_WHITE);
            } else if (column == 0) {
                draw_backspace_icon(bottom_screen_buffer, x + button_width / 2, y + button_height / 2, COLOR_WHITE);
            } else if (column == 1) {
                draw_digit(bottom_screen_buffer, x + 25, y + 7, 0, 2, COLOR_WHITE);
            } else {
                draw_confirm_icon(bottom_screen_buffer, x + button_width / 2, y + 10, COLOR_WHITE);
            }
        }
    }
}

int parse_page_input(const char* input) {
    if (*input == '\0') return 0;

    errno = 0;
    char* end = nullptr;
    const long page = strtol(input, &end, 10);
    if (errno == ERANGE || *end != '\0' || page < 1 ||
        page > chapters[current_chapter_index].page_count) {
        return 0;
    }
    return static_cast<int>(page);
}

int page_input_dialog() {
    char input[PAGE_INPUT_CAPACITY] = {};
    int input_length = 0;
    bool invalid = false;

    draw_page_dialog(input, false);
    present_bottom_dialog();

    while (pmMainLoop()) {
        scanKeys();
        const u32 keys_down = keysDown();

        if (keys_down & KEY_B) {
            return 0;
        }

        bool redraw = false;
        bool confirm = (keys_down & KEY_A) != 0;

        if (keys_down & KEY_TOUCH) {
            touchPosition touch = {};
            if (touchRead(&touch)) {
                constexpr int keypad_x = 32;
                constexpr int keypad_y = 54;
                constexpr int button_width = 60;
                constexpr int button_height = 28;
                constexpr int gap_x = 6;
                constexpr int gap_y = 4;

                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 3; ++column) {
                        const int x = keypad_x + column * (button_width + gap_x);
                        const int y = keypad_y + row * (button_height + gap_y);
                        if (touch.px < x || touch.px >= x + button_width ||
                            touch.py < y || touch.py >= y + button_height) {
                            continue;
                        }

                        if (row < 3) {
                            if (input_length < PAGE_INPUT_CAPACITY - 1) {
                                input[input_length++] = static_cast<char>('1' + row * 3 + column);
                                input[input_length] = '\0';
                                redraw = true;
                            }
                        } else if (column == 0) {
                            if (input_length > 0) {
                                input[--input_length] = '\0';
                                redraw = true;
                            }
                        } else if (column == 1) {
                            if (input_length < PAGE_INPUT_CAPACITY - 1) {
                                input[input_length++] = '0';
                                input[input_length] = '\0';
                                redraw = true;
                            }
                        } else {
                            confirm = true;
                        }
                    }
                }
            }
        }

        if (confirm) {
            const int selected_page = parse_page_input(input);
            if (selected_page > 0) {
                return selected_page;
            }
            invalid = true;
            redraw = true;
        } else if (redraw) {
            invalid = false;
        }

        if (redraw) {
            draw_page_dialog(input, invalid);
            present_bottom_dialog();
        } else {
            swiWaitForVBlank();
        }
    }
    exit(0);
}

void initialize_graphics() {
    lcdMainOnBottom();

    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    bottom_background = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    top_background = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
}

void wait_for_picker_acknowledgement() {
    while (pmMainLoop()) {
        scanKeys();
        if (keysDown() & (KEY_A | KEY_B)) {
            return;
        }
        swiWaitForVBlank();
    }
    exit(0);
}

void open_runtime_book_picker() {
    consoleDemoInit();
    const int selected_book = choose_book(current_book_index, true);
    if (selected_book >= 0 && selected_book != current_book_index) {
        iprintf("\x1b[2J\x1b[1;1H");
        iprintf("Readmanna\n\nLoading %.28s...\n", books[selected_book].title);
        if (!switch_book_transactionally(selected_book)) {
            iprintf("\nThe selected book or its saved page could not be loaded.\n");
            iprintf("The previous book is still open.\n\nA/B: return");
            wait_for_picker_acknowledgement();
        }
    }

    initialize_graphics();
    keysSetRepeat(10, 2);
    present_reader_view();
}

} // namespace

int main() {
    if (!isDSiMode()) {
        fatal_error(
            "DSi mode is required.",
            "Launch the ROM as DSi-enhanced software."
        );
    }
    setCpuClock(true);

    consoleDemoInit();
    iprintf("Readmanna\n\nMounting SD card...\n");
    if (!fatInitDefault()) {
        fatal_error(
            "SD card initialization failed.",
            "Launch through a DSi SD-capable loader."
        );
    }

    iprintf("Scanning book manifests...\n");
    if (!scan_books()) {
        fatal_error(
            "No valid books were found.",
            "Copy readmanna/books to the SD root."
        );
    }

    const int selected_book = choose_book();
    iprintf("\x1b[2J\x1b[1;1H");
    iprintf("Readmanna\n\nLoading %.28s...\n", books[selected_book].title);
    if (!activate_book(selected_book)) {
        fatal_error(
            "The selected book is invalid.",
            "Export it again with the current GUI."
        );
    }
    if (!load_saved_or_first_page()) {
        fatal_error(
            "The saved or first page could not be loaded.",
            "Check the SD card files and page sizes."
        );
    }

    initialize_graphics();
    keysSetRepeat(10, 2);
    present_reader_view();
    DragState drag_state = {};
    int pending_zoom_direction = 0;
    int pending_zoom_frames = 0;
    bool book_shortcut_latched = false;

    while (pmMainLoop()) {
        scanKeys();
        const u32 keys_down = keysDown();
        const u32 keys_held = keysHeld();
        const u32 repeated_keys = keysDownRepeat();
        bool redraw = false;
        bool view_rebased = false;
        bool dialog_opened = false;

        const bool book_chord_held =
            (keys_held & BOOK_PICKER_CHORD) == BOOK_PICKER_CHORD;
        if (!book_chord_held) {
            book_shortcut_latched = false;
        }
        if (book_chord_held && !book_shortcut_latched) {
            book_shortcut_latched = true;
            pending_zoom_direction = 0;
            pending_zoom_frames = 0;
            drag_state.active = false;
            open_runtime_book_picker();
            continue;
        }

        bool zoom_key_scheduled = false;
        if (keys_down & KEY_SELECT) {
            pending_zoom_direction = 1;
            pending_zoom_frames = BOOK_SHORTCUT_GRACE_FRAMES;
            zoom_key_scheduled = true;
        } else if (keys_down & KEY_START) {
            pending_zoom_direction = -1;
            pending_zoom_frames = BOOK_SHORTCUT_GRACE_FRAMES;
            zoom_key_scheduled = true;
        }
        if ((keys_down & ~BOOK_PICKER_CHORD) != 0) {
            pending_zoom_direction = 0;
            pending_zoom_frames = 0;
        } else if (pending_zoom_direction != 0 && !zoom_key_scheduled) {
            --pending_zoom_frames;
        }

        if (pending_zoom_direction > 0 && pending_zoom_frames <= 0) {
            const int previous_zoom = zoom_index;
            if (zoom_index + 1 < ZOOM_LEVEL_COUNT && ensure_full_page_loaded()) {
                change_zoom(1);
            }
            if (zoom_index != previous_zoom) {
                redraw = true;
                view_rebased = true;
            }
            pending_zoom_direction = 0;
        } else if (pending_zoom_direction < 0 && pending_zoom_frames <= 0) {
            const int previous_zoom = zoom_index;
            change_zoom(-1);
            if (zoom_index != previous_zoom) {
                redraw = true;
                view_rebased = true;
            }
            pending_zoom_direction = 0;
        }
        if (keys_down & KEY_A) {
            if (reset_zoom_and_center_view()) {
                redraw = true;
                view_rebased = true;
            }
        }

        if (keys_down & KEY_X) {
            if (select_adjacent_page(1)) {
                redraw = true;
                view_rebased = true;
            }
        } else if (keys_down & KEY_Y) {
            if (select_adjacent_page(-1)) {
                redraw = true;
                view_rebased = true;
            }
        }

        if (keys_down & KEY_L) {
            if (rotate_view(1)) {
                redraw = true;
                view_rebased = true;
            }
        } else if (keys_down & KEY_R) {
            if (rotate_view(-1)) {
                redraw = true;
                view_rebased = true;
            }
        }

        if (keys_down & KEY_B) {
            drag_state.active = false;
            const int selected_page = page_input_dialog();
            if (selected_page > 0 && selected_page != current_page) {
                select_page(current_chapter_index, selected_page);
            }
            redraw = true;
            view_rebased = true;
            dialog_opened = true;
        }

        if (view_rebased) {
            drag_state.active = false;
        }

        if (!dialog_opened && (keys_held & KEY_TOUCH)) {
            redraw = drag_view(&drag_state) || redraw;
        } else {
            drag_state.active = false;
            if (!dialog_opened && !view_rebased) {
                redraw = pan_view(
                    repeated_keys & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)
                ) || redraw;
            }
        }

        if (redraw) {
            present_reader_view();
        } else {
            swiWaitForVBlank();
        }
    }

    return 0;
}
