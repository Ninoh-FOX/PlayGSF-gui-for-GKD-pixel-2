#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include "VBA/psftag.h"
}

#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define FONT_SIZE 24
#define MUSIC_ROOT "/roms/music/GBA"

struct Entry {
    std::string name;
    bool is_dir;
};

enum Mode { MODE_LIST, MODE_PLAYBACK };
enum LoopMode { LOOP_OFF, LOOP_ONE, LOOP_ALL };

Mode mode = MODE_LIST;
LoopMode loop_mode = LOOP_ALL;
std::vector<Entry> entries;
std::string current_path = MUSIC_ROOT;
int selected_index = 0;
int scroll_offset = 0;

pid_t playgsf_pid = -1;
bool paused = false;
bool screen_off = false;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
TTF_Font* font = nullptr;

const int TRIGGER_THRESHOLD = 16000;
bool l2_prev = false, r2_prev = false;

// NUEVAS BANDERAS DE CONTROL SALTO MANUAL
bool manual_switch = false;
bool manual_forward = true;

// Estructura de metadatos de pista
struct TrackMetadata {
    std::string filename, title, artist, game, year, copyright, gsf_by, length;
};

// Clamp manual (C++14)
static void clamp_index(int& idx, int low, int high) {
    if (idx < low) idx = low;
    if (idx > high) idx = high;
}

bool is_directory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool is_valid_music(const std::string& fname) {
    auto pos = fname.find_last_of('.');
    if (pos == std::string::npos) return false;
    std::string ext = fname.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".minigsf";
}

void list_directory(const std::string& path, bool reset_selection = true) {
    entries.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full_path = path + "/" + name;
        bool dir_flag = is_directory(full_path);
        if (dir_flag || is_valid_music(name)) {
            entries.emplace_back(Entry{name, dir_flag});
        }
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b){
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
    if (reset_selection) {
        selected_index = 0;
        scroll_offset = 0;
    }
    clamp_index(selected_index, 0, (int)entries.size() - 1);
}

void kill_playgsf() {
    if (playgsf_pid > 0) {
        kill(playgsf_pid, SIGKILL);
        paused = false;
    }
}

bool launch_playgsf(const std::string& filepath) {
    if (playgsf_pid != -1) return false;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/playgsf", "playgsf", "-c", "-s", "-q", filepath.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        playgsf_pid = pid;
        paused = false;
        return true;
    }
    return false;
}

int find_next_track(int current, bool forward = true) {
    int idx = current;
    int size = (int)entries.size();
    if (size == 0) return -1;
    do {
        idx = forward ? (idx + 1) % size : (idx - 1 + size) % size;
        if (!entries[idx].is_dir && is_valid_music(entries[idx].name))
            return idx;
    } while (idx != current);
    return current;
}

// Parse de etiquetas length en formato "m:ss", "ss" o "ss.xxx"
int parse_length(const std::string& str) {
    if (str.empty()) return 0;
    int min = 0, sec = 0;
    size_t colon = str.find(':');
    size_t dot = str.find('.');
    try {
        if (colon != std::string::npos) {
            min = std::stoi(str.substr(0, colon));
            sec = std::stoi(str.substr(colon + 1));
            return min * 60 + sec;
        } else if (dot != std::string::npos) {
            return std::stoi(str.substr(0, dot));
        }
        return std::stoi(str);
    } catch(...) {
        return 0;
    }
}

bool read_metadata(const std::string& file, TrackMetadata& out) {
    char tag[50001] = {0};
    if (psftag_readfromfile((void*)tag, file.c_str())) return false;
    char buf[512] = {0};
    out.filename = file;
    if (psftag_getvar(tag, "title", buf, sizeof(buf)) == 0) out.title = buf;
    if (psftag_getvar(tag, "artist", buf, sizeof(buf)) == 0) out.artist = buf;
    if (psftag_getvar(tag, "game", buf, sizeof(buf)) == 0) out.game = buf;
    if (psftag_getvar(tag, "year", buf, sizeof(buf)) == 0) out.year = buf;
    if (psftag_getvar(tag, "copyright", buf, sizeof(buf)) == 0) out.copyright = buf;
    if (psftag_getvar(tag, "gsfby", buf, sizeof(buf)) == 0) out.gsf_by = buf;
    if (psftag_getvar(tag, "length", buf, sizeof(buf)) == 0) out.length = buf;
    return true;
}

void render_text(const std::string& text, int x, int y, SDL_Color color) {
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_Rect dst = {x, y, 0, 0};
    SDL_QueryTexture(tex, nullptr, nullptr, &dst.w, &dst.h);
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
}

void draw_list() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_Color white = {255,255,255,255};
    SDL_Color highlight = {255,255,0,255};
    SDL_Color dir_color = {0,255,255,255};
    int lh = TTF_FontLineSkip(font);
    int help_h = lh * 4;
    int max_lines = (SCREEN_HEIGHT - help_h) / lh - 1;
    if (max_lines < 1) max_lines = 1;
    int total = (int)entries.size();
    clamp_index(selected_index, 0, total > 0 ? total - 1 : 0);
    if (total == 0) {
        render_text("No items found", 30, 50, white);
        SDL_RenderPresent(renderer);
        return;
    }
    if (selected_index <= max_lines / 2)
        scroll_offset = 0;
    else if (selected_index >= total - max_lines / 2)
        scroll_offset = std::max(0, total - max_lines);
    else
        scroll_offset = selected_index - max_lines / 2;
    render_text("Directory: " + current_path, 5, 2, white);
    int y = lh + 5;
    for (int i = scroll_offset; i < total && i < scroll_offset + max_lines; ++i) {
        SDL_Color color = (i == selected_index) ? highlight : (entries[i].is_dir ? dir_color : white);
        std::string prefix = entries[i].is_dir ? "[DIR] " : " ";
        render_text(prefix + entries[i].name, 10, y, color);
        y += lh;
    }
    int hy = SCREEN_HEIGHT - 60;
    render_text("A: Play/Enter  B: Back  L1/R1: Jump", 10, hy + lh*0, white);
    render_text("SL: Exit  Menu: Lock", 10, hy + lh*1, white);

    SDL_RenderPresent(renderer);
}

void draw_playback(const TrackMetadata& meta, int elapsed) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Color green = {0, 255, 0, 255};
    SDL_Color orange = {255, 165, 0, 255};
    int y = 20;
    render_text("Now Playing...", 20, y, green);
    y += 40;

    if (!meta.game.empty())        { render_text("Game: ", 20, y, green); render_text(meta.game, 100, y, orange); y += 30; }
    if (!meta.title.empty())       { render_text("Title: ", 20, y, green); render_text(meta.title, 100, y, orange); y += 30; }
    if (!meta.artist.empty())      { render_text("Artist: ", 20, y, green); render_text(meta.artist, 100, y, orange); y += 30; }
    if (!meta.length.empty()) {
        std::string length_no_decimal = meta.length; size_t dot = length_no_decimal.find('.'); if (dot != std::string::npos) length_no_decimal = length_no_decimal.substr(0, dot);
        render_text("Length: ", 20, y, green); render_text(length_no_decimal, 120, y, orange); y += 30;
    }
    char buf[32]; snprintf(buf, sizeof(buf), "%02d:%02d", elapsed/60, elapsed%60);
    render_text("Elapsed: ", 20, y, green); render_text(buf, 140, y, orange); y += 30;
    if (!meta.year.empty())        { render_text("Year: ", 20, y, green); render_text(meta.year, 100, y, orange); y += 30; }
    if (!meta.gsf_by.empty())      { render_text("GSF By: ", 20, y, green); render_text(meta.gsf_by, 120, y, orange); y += 30; }
    if (!meta.copyright.empty())   { render_text("Copyright: ", 20, y, green); render_text(meta.copyright, 160, y, orange); y += 30; }
    std::string looptxt = (loop_mode == LOOP_ALL) ? "ALL" : (loop_mode == LOOP_ONE) ? "ONE" : "OFF";
    render_text("Loop: ", 500, SCREEN_HEIGHT-100, green);
    render_text(looptxt, 570, SCREEN_HEIGHT-100, orange);

    render_text("B:Back  L2/R2:Prev/Next  Y:Loop Mode  Menu:Lock", 10, SCREEN_HEIGHT - 70, green);
    render_text("ST:Pause  SL:exit", 10, SCREEN_HEIGHT - 40, green);
    SDL_RenderPresent(renderer);
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError()); SDL_Quit(); return 1; }
    window = SDL_CreateWindow("playgsf selector", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
    if (!window) { fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError()); TTF_Quit(); SDL_Quit(); return 1; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) { fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError()); SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); return 1; }
    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", FONT_SIZE);
    if (!font) { fprintf(stderr, "TTF_OpenFont error\n"); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); return 1; }
    SDL_GameController* controller = nullptr;
    if (SDL_NumJoysticks() > 0) controller = SDL_GameControllerOpen(0);

    TrackMetadata current_meta;
    int track_seconds = 0;
    using clock_type = std::chrono::steady_clock;
    auto playback_start = clock_type::now();
    int elapsed_seconds = 0;

    list_directory(current_path, true);
    draw_list();
    bool running = true;
    SDL_Event e;

    while (running) {
        SDL_Delay(16);

        // ---- CONTROL DEL FIN DE PISTA y CAMBIO CENTRALIZADO ----
        if (playgsf_pid > 0 && !paused) {
            int status;
            pid_t ret = waitpid(playgsf_pid, &status, WNOHANG);
            if (ret == playgsf_pid) {
                playgsf_pid = -1;
                if (mode == MODE_PLAYBACK) {
                    if (manual_switch) {
                        int next_track = find_next_track(selected_index, manual_forward);
                        if (next_track != selected_index) selected_index = next_track;
                        manual_switch = false;
                        std::string filepath = current_path + "/" + entries[selected_index].name;
                        if (read_metadata(filepath, current_meta)) {
                            track_seconds = parse_length(current_meta.length);
                            playback_start = clock_type::now();
                        }
                        launch_playgsf(filepath);
                        draw_playback(current_meta, 0);
                        mode = MODE_PLAYBACK; paused = false;
                    } else {
                        // FIN DE PISTA AUTOMÁTICO
                        if (track_seconds > 0) {
                            if (loop_mode == LOOP_OFF) {
                                mode = MODE_LIST;
                                draw_list();
                            } else if (loop_mode == LOOP_ONE) {
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta)) {
                                    track_seconds = parse_length(current_meta.length);
                                    playback_start = clock_type::now();
                                }
                                launch_playgsf(filepath);
                                draw_playback(current_meta, 0);
                                mode = MODE_PLAYBACK; paused = false;
                            } else if (loop_mode == LOOP_ALL) {
                                int next_track = find_next_track(selected_index, true);
                                if (next_track != selected_index) selected_index = next_track;
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta)) {
                                    track_seconds = parse_length(current_meta.length);
                                    playback_start = clock_type::now();
                                }
                                launch_playgsf(filepath);
                                draw_playback(current_meta, 0);
                                mode = MODE_PLAYBACK; paused = false;
                            }
                        }
                    }
                }
            }
        }

        // ------ CONTROL DE TIEMPO: MATAR PROCESO si termina -----
        if (mode == MODE_PLAYBACK && playgsf_pid > 0 && !paused) {
            auto now = clock_type::now();
            elapsed_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(now - playback_start).count();
            if (loop_mode == LOOP_OFF) {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            } else if (loop_mode == LOOP_ONE) {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            } else {
            if (track_seconds > 0 && elapsed_seconds >= track_seconds + 5) {
                manual_switch = false; // Fin natural
                kill_playgsf(); // Solo matar proceso, waitpid central decide siguiente acción
                }
            }
            draw_playback(current_meta, elapsed_seconds);
        }

        // ------ CONTROL DE GATILLOS (L2/R2): SOLO MARCAR Y MATAR -------
        if (controller && playgsf_pid > 0 && mode == MODE_PLAYBACK) {
            Sint16 l2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            Sint16 r2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            bool l2_pressed = (l2 > TRIGGER_THRESHOLD);
            bool r2_pressed = (r2 > TRIGGER_THRESHOLD);
            if (l2_pressed && !l2_prev) { manual_switch = true; manual_forward = false; kill_playgsf(); }
            if (r2_pressed && !r2_prev) { manual_switch = true; manual_forward = true; kill_playgsf(); }
            l2_prev = l2_pressed; r2_prev = r2_pressed;
        }

        // --------------- MANEJO DE EVENTOS SDL ---------------
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                    if (!screen_off) {
                        system("wlr-randr --output DSI-1 --off");
                        FILE* f = fopen("/sys/class/backlight/backlight/bl_power", "w");
                        if (f) { fprintf(f, "1\n"); fclose(f); }
                        screen_off = true;
                    } else {
                        system("wlr-randr --output DSI-1 --on");
                        FILE* f = fopen("/sys/class/backlight/backlight/bl_power", "w");
                        if (f) { fprintf(f, "0\n"); fclose(f); }
                        screen_off = false;
                        if (mode == MODE_LIST) draw_list();
                        else draw_playback(current_meta, elapsed_seconds);
                    }
                    SDL_Delay(60); continue;
                }
                if (screen_off) continue;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) running = false;

                if (mode == MODE_PLAYBACK) {
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_B:
                            kill_playgsf(); mode = MODE_LIST; draw_list(); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                            manual_switch = true; manual_forward = false; kill_playgsf(); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                            manual_switch = true; manual_forward = true; kill_playgsf(); break;
                        case SDL_CONTROLLER_BUTTON_Y:
                            loop_mode = static_cast<LoopMode>((loop_mode + 1) % 3);
                            draw_playback(current_meta, elapsed_seconds); break;
                        case SDL_CONTROLLER_BUTTON_START:
                            if (playgsf_pid > 0) {
                                if (!paused) { kill(playgsf_pid, SIGSTOP); paused = true; }
                                else { kill(playgsf_pid, SIGCONT); paused = false; }
                            }
                            break;
                    }
                } else if (mode == MODE_LIST) {
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (selected_index > 0) selected_index--; draw_list(); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (selected_index < (int)entries.size() - 1) selected_index++; draw_list(); break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: selected_index -= 10; if (selected_index < 0) selected_index = 0; draw_list(); break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: selected_index += 10; if (selected_index >= (int)entries.size()) selected_index = (int)entries.size() - 1; draw_list(); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: { int prev = find_next_track(selected_index, false); if (prev != selected_index) { selected_index = prev; draw_list(); } break; }
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: { int next = find_next_track(selected_index, true); if (next != selected_index) { selected_index = next; draw_list(); } break; }
                        case SDL_CONTROLLER_BUTTON_A: {
                            if (selected_index >= 0 && selected_index < (int)entries.size()) {
                                Entry& sel = entries[selected_index];
                                if (sel.is_dir) {
                                    current_path += (current_path == "/" ? "" : "/") + sel.name;
                                    list_directory(current_path, true);
                                    draw_list();
                                } else {
                                    std::string filepath = current_path + "/" + sel.name;
                                    if (read_metadata(filepath, current_meta)) {
                                        track_seconds = parse_length(current_meta.length); playback_start = clock_type::now();
                                        draw_playback(current_meta, 0);
                                    }
                                    launch_playgsf(filepath);
                                    mode = MODE_PLAYBACK; paused = false;
                                }
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_B:
                            if (current_path != MUSIC_ROOT) {
                                size_t pos = current_path.find_last_of('/');
                                current_path = (pos == std::string::npos || current_path == MUSIC_ROOT) ? MUSIC_ROOT : current_path.substr(0, pos);
                                list_directory(current_path, true); draw_list();
                            }
                            break;
                    }
                }
            }
        }
    }

    kill_playgsf();
    if (controller) SDL_GameControllerClose(controller);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
