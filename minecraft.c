#define _DEFAULT_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <termios.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <omp.h>

#define X_BLOCKS 32
#define Y_BLOCKS 32
#define Z_BLOCKS 16

#define EYE_HEIGHT 1.4f
#define PLAYER_HEIGHT 1.6f
#define PLAYER_RADIUS 0.25f
#define VIEW_HEIGHT 0.7f
#define VIEW_WIDTH 1.0f
#define BLOCK_BORDER_SIZE 0.05f

#define GRAVITY 18.0f
#define JUMP_FORCE 6.5f

#define HOTBAR_SLOTS 9

typedef enum {
    BLOCK_AIR = 0,
    BLOCK_GRASS,
    BLOCK_DIRT,
    BLOCK_STONE,
    BLOCK_WOOD,
    BLOCK_LEAVES,
    BLOCK_WATER,
    BLOCK_BEDROCK,
    BLOCK_BORDER,
    BLOCK_COUNT
} BlockType;


typedef struct {
    char name[12];
    char symbol;          
    char far_symbol;      
    const char* color_code;     
    const char* far_color_code; 
    int solid;
} BlockDef;

static const BlockDef block_defs[BLOCK_COUNT] = {
    [BLOCK_AIR]     = { "Air",     ' ', ' ', "\x1b[0m",           "\x1b[0m",           0 },
    [BLOCK_GRASS]   = { "Grass",   '%', ':', "\x1b[38;5;106m",    "\x1b[38;5;58m",     1 }, 
    [BLOCK_DIRT]    = { "Dirt",    ':', '.', "\x1b[38;5;137m",    "\x1b[38;5;94m",     1 }, 
    [BLOCK_STONE]   = { "Stone",   '=', '-', "\x1b[38;5;244m",    "\x1b[38;5;239m",    1 }, 
    [BLOCK_WOOD]    = { "Wood",    '+', ':', "\x1b[38;5;172m",    "\x1b[38;5;130m",    1 }, 
    [BLOCK_LEAVES]  = { "Leaves",  '*', '.', "\x1b[38;5;28m",     "\x1b[38;5;22m",     1 }, 
    [BLOCK_WATER]   = { "Water",   '~', '-', "\x1b[38;5;31m",     "\x1b[38;5;23m",     1 }, 
    [BLOCK_BEDROCK] = { "Bedrock", '@', '%', "\x1b[38;5;237m",    "\x1b[38;5;234m",    1 },
    [BLOCK_BORDER]  = { "Border",  '.', '.', "\x1b[38;5;240m",    "\x1b[38;5;236m",    0 } 
};

typedef struct Vector {
    float x;
    float y;
    float z;
} vect;

typedef struct Vector2 {
    float psi; 
    float phi; 
} vect2;

typedef struct Vector_vector2 {
    vect pos;
    vect2 view;
} player_pos_view;

typedef struct {
    uint8_t block_type;
    uint8_t is_border;
    float dist;
} RenderCell;

typedef struct {
    uint8_t type;
    int count;
} ItemStack;

typedef struct {
    ItemStack slots[HOTBAR_SLOTS];
    int active_slot;
} Inventory;


static struct termios old_termios, new_termios;
static char keystate[256] = { 0 };


static vect** g_dir_cache = NULL;
static int g_cached_rows = 0;
static int g_cached_cols = 0;
static float g_cached_psi = -999.0f;
static float g_cached_phi = -999.0f;


static char* g_out_buf = NULL;
static size_t g_out_buf_cap = 0;


static int g_term_cols = 100;
static int g_term_rows = 40;

static float g_vel_z = 0.0f;
static int g_on_ground = 0;

void init_terminal() {
    tcgetattr(STDIN_FILENO, &old_termios);
    new_termios = old_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    
    write(STDOUT_FILENO, "\x1b[?25l\x1b[2J", 10);
    fflush(stdout);
}

void restore_terminal() {
   
    write(STDOUT_FILENO, "\x1b[?25h\x1b[0m\n", 10);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
}

void process_input() {
    char c;
    for (int i = 0; i < 256; i++) {
        keystate[i] = 0;
    }

    while (read(STDIN_FILENO, &c, 1) > 0) {
        unsigned char uc = (unsigned char)c;
        keystate[uc] = 1;
    }
}

int is_key_pressed(char key) {
    return keystate[(unsigned char)key];
}

void update_terminal_size() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        g_term_cols = ws.ws_col;
        g_term_rows = ws.ws_row;
    }
}

RenderCell** init_picture(int rows, int cols) {
    RenderCell** picture = malloc(sizeof(RenderCell*) * rows);
    for (int i = 0; i < rows; i++) {
        picture[i] = malloc(sizeof(RenderCell) * cols);
    }
    return picture;
}

void free_picture(RenderCell** picture, int rows) {
    if (!picture) return;
    for (int i = 0; i < rows; i++) {
        free(picture[i]);
    }
    free(picture);
}

void free_direction_cache() {
    if (!g_dir_cache) return;
    for (int i = 0; i < g_cached_rows; i++) {
        free(g_dir_cache[i]);
    }
    free(g_dir_cache);
    g_dir_cache = NULL;
    g_cached_rows = 0;
    g_cached_cols = 0;
    g_cached_psi = -999.0f;
    g_cached_phi = -999.0f;
}

uint8_t*** init_blocks() {
    uint8_t*** blocks = malloc(sizeof(uint8_t**) * Z_BLOCKS);
    for (int i = 0; i < Z_BLOCKS; i++) {
        blocks[i] = malloc(sizeof(uint8_t*) * Y_BLOCKS);
        for (int j = 0; j < Y_BLOCKS; j++) {
            blocks[i][j] = malloc(sizeof(uint8_t) * X_BLOCKS);
            for (int k = 0; k < X_BLOCKS; k++) {
                blocks[i][j][k] = BLOCK_AIR;
            }
        }
    }
    return blocks;
}

void free_blocks(uint8_t*** blocks) {
    if (!blocks) return;
    for (int i = 0; i < Z_BLOCKS; i++) {
        for (int j = 0; j < Y_BLOCKS; j++) {
            free(blocks[i][j]);
        }
        free(blocks[i]);
    }
    free(blocks);
}

void init_inventory(Inventory* inv) {
    inv->active_slot = 0;
    inv->slots[0] = (ItemStack){ BLOCK_GRASS, 64 };
    inv->slots[1] = (ItemStack){ BLOCK_DIRT, 64 };
    inv->slots[2] = (ItemStack){ BLOCK_STONE, 64 };
    inv->slots[3] = (ItemStack){ BLOCK_WOOD, 32 };
    inv->slots[4] = (ItemStack){ BLOCK_LEAVES, 32 };
    inv->slots[5] = (ItemStack){ BLOCK_WATER, 16 };
    inv->slots[6] = (ItemStack){ BLOCK_AIR, 0 };
    inv->slots[7] = (ItemStack){ BLOCK_AIR, 0 };
    inv->slots[8] = (ItemStack){ BLOCK_AIR, 0 };
}

void process_inventory_input(Inventory* inv) {
    for (int i = 0; i < 9; i++) {
        if (is_key_pressed('1' + i)) {
            inv->active_slot = i;
        }
    }
}

int inventory_add_item(Inventory* inv, uint8_t type) {
    if (type == BLOCK_AIR || type == BLOCK_BEDROCK || type == BLOCK_BORDER) return 0;
    for (int i = 0; i < HOTBAR_SLOTS; i++) {
        if (inv->slots[i].type == type && inv->slots[i].count < 64) {
            inv->slots[i].count++;
            return 1;
        }
    }
    for (int i = 0; i < HOTBAR_SLOTS; i++) {
        if (inv->slots[i].count == 0 || inv->slots[i].type == BLOCK_AIR) {
            inv->slots[i].type = type;
            inv->slots[i].count = 1;
            return 1;
        }
    }
    return 0;
}

uint8_t inventory_consume_active(Inventory* inv) {
    int idx = inv->active_slot;
    if (inv->slots[idx].count > 0 && inv->slots[idx].type != BLOCK_AIR) {
        uint8_t type = inv->slots[idx].type;
        inv->slots[idx].count--;
        if (inv->slots[idx].count == 0) {
            inv->slots[idx].type = BLOCK_AIR;
        }
        return type;
    }
    return BLOCK_AIR;
}

player_pos_view init_posview() {
    player_pos_view posview;
    posview.pos.x = 16.0f;
    posview.pos.y = 16.0f;
    posview.pos.z = 5.0f + EYE_HEIGHT;
    posview.view.phi = 0.0f;
    posview.view.psi = 0.0f;
    return posview;
}

vect angles_to_vect(vect2 angles) {
    vect res;
    res.x = cosf(angles.psi) * cosf(angles.phi);
    res.y = cosf(angles.psi) * sinf(angles.phi);
    res.z = sinf(angles.psi);
    return res;
}

vect vect_add(vect v1, vect v2) {
    vect res = { v1.x + v2.x, v1.y + v2.y, v1.z + v2.z };
    return res;
}

vect vect_scale(float s, vect v) {
    vect res = { s * v.x, s * v.y, s * v.z };
    return res;
}

vect vect_sub(vect v1, vect v2) {
    vect res = { v1.x - v2.x, v1.y - v2.y, v1.z - v2.z };
    return res;
}

void vect_normalize(vect* v) {
    float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len > 0.00001f) {
        v->x /= len;
        v->y /= len;
        v->z /= len;
    }
}

float min(float a, float b) {
    return (a < b) ? a : b;
}

void update_direction_cache(vect2 view, int rows, int cols) {
    if (g_dir_cache != NULL && g_cached_rows == rows && g_cached_cols == cols &&
        fabsf(g_cached_psi - view.psi) < 0.0001f && fabsf(g_cached_phi - view.phi) < 0.0001f) {
        return;
    }

    if (g_dir_cache == NULL || g_cached_rows != rows || g_cached_cols != cols) {
        free_direction_cache();
        g_dir_cache = malloc(sizeof(vect*) * rows);
        for (int i = 0; i < rows; i++) {
            g_dir_cache[i] = malloc(sizeof(vect) * cols);
        }
    }

    g_cached_rows = rows;
    g_cached_cols = cols;
    g_cached_psi = view.psi;
    g_cached_phi = view.phi;

    vect2 v_temp = view;
    v_temp.psi -= VIEW_HEIGHT / 2.0f;
    vect screen_down = angles_to_vect(v_temp);
    v_temp.psi += VIEW_HEIGHT;
    vect screen_up = angles_to_vect(v_temp);
    v_temp.psi -= VIEW_HEIGHT / 2.0f;
    v_temp.phi -= VIEW_WIDTH / 2.0f;
    vect screen_left = angles_to_vect(v_temp);
    v_temp.phi += VIEW_WIDTH;
    vect screen_right = angles_to_vect(v_temp);

    vect screen_mid_vert = vect_scale(0.5f, vect_add(screen_up, screen_down));
    vect screen_mid_hor = vect_scale(0.5f, vect_add(screen_left, screen_right));
    vect mid_to_left = vect_sub(screen_left, screen_mid_hor);
    vect mid_to_up = vect_sub(screen_up, screen_mid_vert);

    for (int y_pix = 0; y_pix < rows; y_pix++) {
        for (int x_pix = 0; x_pix < cols; x_pix++) {
            vect tmp = vect_add(vect_add(screen_mid_hor, mid_to_left), mid_to_up);
            float x_ratio = ((float)x_pix / (cols > 1 ? cols - 1 : 1)) * 2.0f;
            float y_ratio = ((float)y_pix / (rows > 1 ? rows - 1 : 1)) * 2.0f;
            tmp = vect_sub(tmp, vect_scale(x_ratio, mid_to_left));
            tmp = vect_sub(tmp, vect_scale(y_ratio, mid_to_up));
            vect_normalize(&tmp);
            g_dir_cache[y_pix][x_pix] = tmp;
        }
    }
}

int ray_outside(vect pos) {
    if (pos.x >= X_BLOCKS || pos.y >= Y_BLOCKS || pos.z >= Z_BLOCKS ||
        pos.x < 0.0f || pos.y < 0.0f || pos.z < 0.0f) {
        return 1;
    }
    return 0;
}

int on_block_border(vect pos) {
    int cnt = 0;
    if (fabsf(pos.x - roundf(pos.x)) < BLOCK_BORDER_SIZE) cnt++;
    if (fabsf(pos.y - roundf(pos.y)) < BLOCK_BORDER_SIZE) cnt++;
    if (fabsf(pos.z - roundf(pos.z)) < BLOCK_BORDER_SIZE) cnt++;
    return (cnt >= 2);
}

int is_solid_block(uint8_t*** blocks, int x, int y, int z) {
    if (x < 0 || x >= X_BLOCKS || y < 0 || y >= Y_BLOCKS || z < 0 || z >= Z_BLOCKS) {
        return 1; 
    }
    return block_defs[blocks[z][y][x]].solid;
}
RenderCell raytrace(vect pos, vect dir, uint8_t*** blocks) {
    RenderCell cell;
    cell.block_type = BLOCK_AIR;
    cell.is_border = 0;
    cell.dist = 0.0f;

    vect start_pos = pos;
    float eps = 0.01f;

    while (!ray_outside(pos)) {
        int bx = (int)pos.x;
        int by = (int)pos.y;
        int bz = (int)pos.z;

        if (bx >= 0 && bx < X_BLOCKS && by >= 0 && by < Y_BLOCKS && bz >= 0 && bz < Z_BLOCKS) {
            uint8_t bt = blocks[bz][by][bx];
            if (bt != BLOCK_AIR) {
                cell.block_type = bt;
                cell.is_border = on_block_border(pos);
                vect diff = vect_sub(pos, start_pos);
                cell.dist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
                return cell;
            }
        }

        float dist = 2.0f;
        if (dir.x > eps) dist = min(dist, ((int)(pos.x + 1) - pos.x) / dir.x);
        else if (dir.x < -eps) dist = min(dist, ((int)pos.x - pos.x) / dir.x);

        if (dir.y > eps) dist = min(dist, ((int)(pos.y + 1) - pos.y) / dir.y);
        else if (dir.y < -eps) dist = min(dist, ((int)pos.y - pos.y) / dir.y);

        if (dir.z > eps) dist = min(dist, ((int)(pos.z + 1) - pos.z) / dir.z);
        else if (dir.z < -eps) dist = min(dist, ((int)pos.z - pos.z) / dir.z);

        pos = vect_add(pos, vect_scale(dist + eps, dir));
    }

    return cell;
}

void get_picture(RenderCell** picture, player_pos_view posview, uint8_t*** blocks, int rows, int cols) {
    update_direction_cache(posview.view, rows, cols);

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            picture[y][x] = raytrace(posview.pos, g_dir_cache[y][x], blocks);
        }
    }
}


void draw_ascii(RenderCell** picture, int rows, int cols, Inventory* inv, player_pos_view posview) {
    size_t est_size = (size_t)rows * (cols * 25 + 32) + 2048;
    if (g_out_buf == NULL || g_out_buf_cap < est_size) {
        free(g_out_buf);
        g_out_buf_cap = est_size;
        g_out_buf = malloc(g_out_buf_cap);
    }

    char* p = g_out_buf;
    p += sprintf(p, "\x1b[H"); 

    int render_rows = rows - 2;
    if (render_rows < 5) render_rows = rows;

    const char* cur_color = "";

    for (int y = 0; y < render_rows; y++) {
        for (int x = 0; x < cols; x++) {
            RenderCell cell = picture[y][x];
            BlockDef def = block_defs[cell.block_type];
            const char* next_color = def.color_code;
            char ch = def.symbol;

            if (y == render_rows / 2 && x == cols / 2) {
                ch = '+';
                next_color = "\x1b[97;1m"; 
            } else if (cell.is_border && cell.block_type != BLOCK_AIR) {
                ch = '.';
                next_color = "\x1b[38;5;240m"; 
            } else if (cell.dist > 14.0f && cell.block_type != BLOCK_AIR) {
                ch = def.far_symbol;
                next_color = def.far_color_code; 
            }

            if (next_color != cur_color) {
                p += sprintf(p, "%s", next_color);
                cur_color = next_color;
            }
            *p++ = ch;
        }
        p += sprintf(p, "\x1b[0m\x1b[K\n");
        cur_color = "";
    }

    p += sprintf(p, "\x1b[0m\x1b[K");
    p += sprintf(p, "HOTBAR: ");
    for (int i = 0; i < HOTBAR_SLOTS; i++) {
        ItemStack slot = inv->slots[i];
        BlockDef def = block_defs[slot.type];
        if (i == inv->active_slot) {
            p += sprintf(p, "\x1b[1;7m[%d:%s %d]\x1b[0m ", i + 1, def.name, slot.count);
        } else {
            p += sprintf(p, "%s[%d:%s %d]\x1b[0m ", def.color_code, i + 1, def.name, slot.count);
        }
    }
    p += sprintf(p, "\x1b[K\n");


    p += sprintf(p, "\x1b[36mPos:(%.1f,%.1f,%.1f) | F/P:Place X:Break Space:Jump 1-9:Slot IJKL:Move WASD:Look Q:Quit\x1b[0m\x1b[K",
                 posview.pos.x, posview.pos.y, posview.pos.z);

    size_t len = p - g_out_buf;
    write(STDOUT_FILENO, g_out_buf, len);
}


void move_player(player_pos_view* posview, float dx, float dy, uint8_t*** blocks) {
    float feet_z = posview->pos.z - EYE_HEIGHT;
    float head_z = feet_z + PLAYER_HEIGHT - 0.05f;

    float target_x = posview->pos.x + dx;
    float check_x = target_x + (dx > 0 ? PLAYER_RADIUS : -PLAYER_RADIUS);
    int col_x = 0;
    for (float cz = feet_z + 0.1f; cz <= head_z; cz += 0.7f) {
        for (float cy = posview->pos.y - PLAYER_RADIUS + 0.05f; cy <= posview->pos.y + PLAYER_RADIUS - 0.05f; cy += 0.3f) {
            if (is_solid_block(blocks, (int)check_x, (int)cy, (int)cz)) {
                col_x = 1;
                break;
            }
        }
        if (col_x) break;
    }
    if (!col_x) {
        posview->pos.x = target_x;
    }


    float target_y = posview->pos.y + dy;
    float check_y = target_y + (dy > 0 ? PLAYER_RADIUS : -PLAYER_RADIUS);
    int col_y = 0;
    for (float cz = feet_z + 0.1f; cz <= head_z; cz += 0.7f) {
        for (float cx = posview->pos.x - PLAYER_RADIUS + 0.05f; cx <= posview->pos.x + PLAYER_RADIUS - 0.05f; cx += 0.3f) {
            if (is_solid_block(blocks, (int)cx, (int)check_y, (int)cz)) {
                col_y = 1;
                break;
            }
        }
        if (col_y) break;
    }
    if (!col_y) {
        posview->pos.y = target_y;
    }
}

void update_physics(player_pos_view* posview, uint8_t*** blocks, float dt) {
    if (is_key_pressed(' ') && g_on_ground) {
        g_vel_z = JUMP_FORCE;
        g_on_ground = 0;
    }

    if (!g_on_ground) {
        g_vel_z -= GRAVITY * dt;
    }

    float feet_z = posview->pos.z - EYE_HEIGHT;
    float new_feet_z = feet_z + g_vel_z * dt;

    if (g_vel_z <= 0.0f) {
        int col_ground = 0;
        float check_z = new_feet_z;
        for (float cx = posview->pos.x - PLAYER_RADIUS + 0.05f; cx <= posview->pos.x + PLAYER_RADIUS - 0.05f; cx += 0.3f) {
            for (float cy = posview->pos.y - PLAYER_RADIUS + 0.05f; cy <= posview->pos.y + PLAYER_RADIUS - 0.05f; cy += 0.3f) {
                if (is_solid_block(blocks, (int)cx, (int)cy, (int)check_z)) {
                    col_ground = 1;
                    break;
                }
            }
            if (col_ground) break;
        }

        if (col_ground) {
            int block_z = (int)check_z;
            posview->pos.z = (float)(block_z + 1) + EYE_HEIGHT;
            g_vel_z = 0.0f;
            g_on_ground = 1;
        } else {
            posview->pos.z = new_feet_z + EYE_HEIGHT;
            g_on_ground = 0;
        }
    } else {
        float head_z = new_feet_z + PLAYER_HEIGHT;
        int col_ceil = 0;
        for (float cx = posview->pos.x - PLAYER_RADIUS + 0.05f; cx <= posview->pos.x + PLAYER_RADIUS - 0.05f; cx += 0.3f) {
            for (float cy = posview->pos.y - PLAYER_RADIUS + 0.05f; cy <= posview->pos.y + PLAYER_RADIUS - 0.05f; cy += 0.3f) {
                if (is_solid_block(blocks, (int)cx, (int)cy, (int)head_z)) {
                    col_ceil = 1;
                    break;
                }
            }
            if (col_ceil) break;
        }

        if (col_ceil) {
            g_vel_z = 0.0f;
        } else {
            posview->pos.z = new_feet_z + EYE_HEIGHT;
            g_on_ground = 0;
        }
    }
}

void update_pos_view(player_pos_view* posview, uint8_t*** blocks, float dt) {
    float move_speed = 4.5f;
    float tilt_eps = 0.08f;

    if (is_key_pressed('w')) posview->view.psi += tilt_eps;
    if (is_key_pressed('s')) posview->view.psi -= tilt_eps;
    if (is_key_pressed('d')) posview->view.phi += tilt_eps;
    if (is_key_pressed('a')) posview->view.phi -= tilt_eps;

    if (posview->view.psi > 1.4f) posview->view.psi = 1.4f;
    if (posview->view.psi < -1.4f) posview->view.psi = -1.4f;

    vect forward;
    forward.x = cosf(posview->view.phi);
    forward.y = sinf(posview->view.phi);
    forward.z = 0.0f;

    vect right;
    right.x = sinf(posview->view.phi);
    right.y = -cosf(posview->view.phi);
    right.z = 0.0f;

    float dx = 0.0f, dy = 0.0f;

    if (is_key_pressed('i')) {
        dx += forward.x * move_speed * dt;
        dy += forward.y * move_speed * dt;
    }
    if (is_key_pressed('k')) {
        dx -= forward.x * move_speed * dt;
        dy -= forward.y * move_speed * dt;
    }
    if (is_key_pressed('j')) {
        dx += right.x * move_speed * dt;
        dy += right.y * move_speed * dt;
    }
    if (is_key_pressed('l')) {
        dx -= right.x * move_speed * dt;
        dy -= right.y * move_speed * dt;
    }

    if (dx != 0.0f || dy != 0.0f) {
        move_player(posview, dx, dy, blocks);
    }

    update_physics(posview, blocks, dt);
}

vect get_current_block(player_pos_view posview, uint8_t*** blocks) {
    vect pos = posview.pos;
    vect dir = angles_to_vect(posview.view);
    float eps = 0.02f;
    float total_dist = 0.0f;
    float max_reach = 6.0f;

    while (!ray_outside(pos) && total_dist < max_reach) {
        int bx = (int)pos.x;
        int by = (int)pos.y;
        int bz = (int)pos.z;
        if (bx >= 0 && bx < X_BLOCKS && by >= 0 && by < Y_BLOCKS && bz >= 0 && bz < Z_BLOCKS) {
            if (blocks[bz][by][bx] != BLOCK_AIR) {
                return pos;
            }
        }
        float dist = 1.0f;
        if (dir.x > eps) dist = min(dist, ((int)(pos.x + 1) - pos.x) / dir.x);
        else if (dir.x < -eps) dist = min(dist, ((int)pos.x - pos.x) / dir.x);

        if (dir.y > eps) dist = min(dist, ((int)(pos.y + 1) - pos.y) / dir.y);
        else if (dir.y < -eps) dist = min(dist, ((int)pos.y - pos.y) / dir.y);

        if (dir.z > eps) dist = min(dist, ((int)(pos.z + 1) - pos.z) / dir.z);
        else if (dir.z < -eps) dist = min(dist, ((int)pos.z - pos.z) / dir.z);

        float step = dist + eps;
        pos = vect_add(pos, vect_scale(step, dir));
        total_dist += step;
    }
    vect invalid = { -1.0f, -1.0f, -1.0f };
    return invalid;
}


void place_block(vect pos, uint8_t*** blocks, uint8_t block) {
    if (pos.x < 0.0f || pos.y < 0.0f || pos.z < 0.0f) return;
    int x = (int)pos.x, y = (int)pos.y, z = (int)pos.z;
    float dists[6];
    dists[0] = fabsf(x + 1 - pos.x);
    dists[1] = fabsf(pos.x - x);
    dists[2] = fabsf(y + 1 - pos.y);
    dists[3] = fabsf(pos.y - y);
    dists[4] = fabsf(z + 1 - pos.z);
    dists[5] = fabsf(pos.z - z);
    int min_idx = 0;
    float mindist = dists[0];
    for (int i = 1; i < 6; i++) {
        if (dists[i] < mindist) {
            mindist = dists[i];
            min_idx = i;
        }
    }
    int target_x = x, target_y = y, target_z = z;
    switch (min_idx) {
        case 0: target_x++; break;
        case 1: target_x--; break;
        case 2: target_y++; break;
        case 3: target_y--; break;
        case 4: target_z++; break;
        case 5: target_z--; break;
    }
    if (target_x >= 0 && target_x < X_BLOCKS &&
        target_y >= 0 && target_y < Y_BLOCKS &&
        target_z >= 0 && target_z < Z_BLOCKS) {
        if (blocks[target_z][target_y][target_x] == BLOCK_AIR) {
            blocks[target_z][target_y][target_x] = block;
        }
    }
}

void generate_tree(uint8_t*** blocks, int tx, int ty, int base_z) {
    int trunk_h = 3 + rand() % 2;
    for (int h = 0; h < trunk_h; h++) {
        int z = base_z + 1 + h;
        if (z < Z_BLOCKS) {
            blocks[z][ty][tx] = BLOCK_WOOD;
        }
    }
    int top_z = base_z + trunk_h;
    for (int dz = 0; dz <= 2; dz++) {
        int radius = (dz == 2) ? 1 : 2;
        for (int dx = -radius; dx <= radius; dx++) {
            for (int dy = -radius; dy <= radius; dy++) {
                int lx = tx + dx;
                int ly = ty + dy;
                int lz = top_z + dz;
                if (lx >= 0 && lx < X_BLOCKS && ly >= 0 && ly < Y_BLOCKS && lz >= 0 && lz < Z_BLOCKS) {
                    if (blocks[lz][ly][lx] == BLOCK_AIR) {
                        blocks[lz][ly][lx] = BLOCK_LEAVES;
                    }
                }
            }
        }
    }
}


void generate_world(uint8_t*** blocks) {
    for (int x = 0; x < X_BLOCKS; x++) {
        for (int y = 0; y < Y_BLOCKS; y++) {
            blocks[0][y][x] = BLOCK_BEDROCK;
            blocks[1][y][x] = BLOCK_STONE;
            blocks[2][y][x] = BLOCK_STONE;
            blocks[3][y][x] = BLOCK_DIRT;
            blocks[4][y][x] = BLOCK_GRASS;
        }
    }

    srand(42);
    for (int i = 0; i < 8; i++) {
        int tx = 3 + rand() % (X_BLOCKS - 6);
        int ty = 3 + rand() % (Y_BLOCKS - 6);
        generate_tree(blocks, tx, ty, 4);
    }
}

int main() {
    init_terminal();
    update_terminal_size();

    int cur_rows = g_term_rows;
    int cur_cols = g_term_cols;
    RenderCell** picture = init_picture(cur_rows, cur_cols);
    uint8_t*** blocks = init_blocks();

    generate_world(blocks);

    Inventory inv;
    init_inventory(&inv);

    player_pos_view posview = init_posview();
    float dt = 0.02f; 

    while (1) {
        process_input();

        if (is_key_pressed('q')) {
            break;
        }

        process_inventory_input(&inv);

        update_terminal_size();
        if (g_term_rows != cur_rows || g_term_cols != cur_cols) {
            free_picture(picture, cur_rows);
            cur_rows = g_term_rows;
            cur_cols = g_term_cols;
            picture = init_picture(cur_rows, cur_cols);
        }

        update_pos_view(&posview, blocks, dt);

        vect current_block = get_current_block(posview, blocks);
        int have_current_block = (current_block.x >= 0.0f);
        int current_block_x = (int)current_block.x;
        int current_block_y = (int)current_block.y;
        int current_block_z = (int)current_block.z;
        uint8_t current_block_type = BLOCK_AIR;

        if (have_current_block) {
            current_block_type = blocks[current_block_z][current_block_y][current_block_x];
            
            if (is_key_pressed('x')) {
                blocks[current_block_z][current_block_y][current_block_x] = BLOCK_AIR;
                inventory_add_item(&inv, current_block_type);
            }

            if (is_key_pressed('f') || is_key_pressed('p')) {
                uint8_t place_type = inventory_consume_active(&inv);
                if (place_type != BLOCK_AIR) {
                    place_block(current_block, blocks, place_type);
                }
            }
        }

        get_picture(picture, posview, blocks, cur_rows, cur_cols);

        draw_ascii(picture, cur_rows, cur_cols, &inv, posview);

        usleep(20000); 
    }

    restore_terminal();
    free_picture(picture, cur_rows);
    free_direction_cache();
    free_blocks(blocks);
    if (g_out_buf) free(g_out_buf);

    return 0;
}