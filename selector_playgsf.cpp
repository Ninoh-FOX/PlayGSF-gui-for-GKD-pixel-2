#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

// Incluye tu cabecera de psftag (asegúrate de que psftag.c esté compilado y enlazado)
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
Mode mode = MODE_LIST;

enum LoopMode { LOOP_OFF, LOOP_ONE, LOOP_ALL };
LoopMode loop_mode = LOOP_ALL;  // Estado por defecto del loop

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

struct TrackMetadata {
    std::string filename;
    std::string title;
    std::string artist;
    std::string game;
    std::string year;
    std::string copyright;
    std::string gsf_by;
    std::string length;
};

// Función clamp manual para C++14
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
        waitpid(playgsf_pid, nullptr, 0);
        playgsf_pid = -1;
        paused = false;
    }
}

bool launch_playgsf(const std::string& filepath) {
    if (playgsf_pid != -1) return false;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/playgsf", "playgsf", "-c", "-s", filepath.c_str(), nullptr);
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
        std::string prefix = entries[i].is_dir ? "[DIR] " : "      ";
        render_text(prefix + entries[i].name, 10, y, color);
        y += lh;
    }
    int hy = SCREEN_HEIGHT - 60;
    render_text("A: Play/Enter  B: Back  L1/R1: Jump", 10, hy + lh*0, white);
    render_text("SL: Exit  Menu: Lock", 10, hy + lh*1, white);

    SDL_RenderPresent(renderer);
}

void draw_playback(const TrackMetadata& meta, int elapsed) {
    SDL_SetRenderDrawColor(renderer, 0,0,0,255);
    SDL_RenderClear(renderer);
    SDL_Color white = {255,255,255,255};
    SDL_Color yellow = {255,255,0,255};
    SDL_Color cyan = {0,255,255,255};
    int y = 20;
    render_text("Now Playing...", 20, y, yellow);
    y += 40;
    if (!meta.game.empty()) { render_text("Game: " + meta.game, 20, y, white); y += 30; }
    if (!meta.title.empty()) { render_text("Title: " + meta.title, 20, y, white); y += 30; }
    if (!meta.artist.empty()) { render_text("Artist: " + meta.artist, 20, y, white); y += 30; }
    if (!meta.length.empty()) { render_text("Length: " + meta.length, 20, y, yellow); y += 30; }
    char buf[32];
    snprintf(buf, sizeof(buf), "Elapsed: %02d:%02d", elapsed / 60, elapsed % 60);
    render_text(buf, 20, y, cyan);
    y += 30;
    if (!meta.year.empty()) { render_text("Year: " + meta.year, 20, y, white); y += 30; }
    if (!meta.gsf_by.empty()) { render_text("GSF By: " + meta.gsf_by, 20, y, white); y += 30; }
    if (!meta.copyright.empty()) { render_text(meta.copyright, 20, y, white); y += 30; }

    // Mostrar modo loop en reproducción también
    std::string loop_text = "Loop: ";
    switch (loop_mode) {
        case LOOP_OFF: loop_text += "OFF"; break;
        case LOOP_ONE: loop_text += "ONE"; break;
        case LOOP_ALL: loop_text += "ALL"; break;
    }
    render_text(loop_text, 500, SCREEN_HEIGHT - 100, yellow);

    render_text("B:Back  L2/R2:Prev/Next  Y:Loop Mode  Menu:Lock", 10, SCREEN_HEIGHT-70, yellow);
    render_text("ST:Pause  SL:exit", 10, SCREEN_HEIGHT-40, yellow);
    SDL_RenderPresent(renderer);
}

int main() {
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init error: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    window = SDL_CreateWindow("playgsf selector", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", FONT_SIZE);
    if (!font) {
        fprintf(stderr, "TTF_OpenFont error: cannot open font\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_GameController* controller = nullptr;
    if (SDL_NumJoysticks() > 0) controller = SDL_GameControllerOpen(0);
    TrackMetadata current_meta;
    using clock_type = std::chrono::steady_clock;
    auto playback_start = clock_type::now();
    int elapsed_seconds = 0;
    list_directory(current_path, true);
    draw_list();
    bool running = true;
    SDL_Event e;
    while (running) {
        SDL_Delay(16);
        if (playgsf_pid > 0 && !paused) {
            int status;
            pid_t ret = waitpid(playgsf_pid, &status, WNOHANG);
            if (ret == playgsf_pid) {
                playgsf_pid = -1;
                // Manejo del loop aquí:
                if (loop_mode == LOOP_OFF) {
                    // Parar reproducción, volver al listado
                    mode = MODE_LIST;
                    draw_list();
                } else {
                    int next_track = (loop_mode == LOOP_ONE) ? selected_index : find_next_track(selected_index, true);
                    if (next_track != selected_index)
                        selected_index = next_track;
                    std::string filepath = current_path + "/" + entries[selected_index].name;
                    if (read_metadata(filepath, current_meta))
                        playback_start = clock_type::now();
                    launch_playgsf(filepath);
                    if (mode == MODE_PLAYBACK)
                        draw_playback(current_meta, 0);
                    else
                        draw_list();
                }
            }
        }
        if (mode == MODE_PLAYBACK && playgsf_pid > 0 && !paused) {
            auto now = clock_type::now();
            elapsed_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(now - playback_start).count();
            draw_playback(current_meta, elapsed_seconds);
        }
        if (controller && playgsf_pid > 0) {
            Sint16 l2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            Sint16 r2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            bool l2_pressed = (l2 > TRIGGER_THRESHOLD);
            bool r2_pressed = (r2 > TRIGGER_THRESHOLD);

            if (l2_pressed && !l2_prev) {
                int prev_track = find_next_track(selected_index, false);
                if (prev_track != selected_index) {
                    selected_index = prev_track;
                    std::string filepath = current_path + "/" + entries[selected_index].name;
                    if (read_metadata(filepath, current_meta))
                        playback_start = clock_type::now();
                    kill_playgsf();
                    launch_playgsf(filepath);
                    paused = false;
                    if (mode == MODE_LIST) draw_list();
                    else draw_playback(current_meta, 0);
                }
            }
            if (r2_pressed && !r2_prev) {
                int next_track = find_next_track(selected_index, true);
                if (next_track != selected_index) {
                    selected_index = next_track;
                    std::string filepath = current_path + "/" + entries[selected_index].name;
                    if (read_metadata(filepath, current_meta))
                        playback_start = clock_type::now();
                    kill_playgsf();
                    launch_playgsf(filepath);
                    paused = false;
                    if (mode == MODE_LIST) draw_list();
                    else draw_playback(current_meta, 0);
                }
            }
            l2_prev = l2_pressed;
            r2_prev = r2_pressed;
        }
        // Evento botón Y para cambiar loop
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
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
                    continue;
                }
                if (screen_off) continue;
                
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                    running = false;
                }

                if (mode == MODE_PLAYBACK && e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                    loop_mode = static_cast<LoopMode>((loop_mode + 1) % 3);
                    draw_playback(current_meta, elapsed_seconds);
                    continue;
                }

                if (mode == MODE_LIST) {
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            if (selected_index > 0) selected_index--;
                            draw_list();
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            if (selected_index < (int)entries.size() - 1) selected_index++;
                            draw_list();
                            break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                            selected_index -= 10;
                            if (selected_index < 0) selected_index = 0;
                            draw_list();
                            break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                            selected_index += 10;
                            if (selected_index >= (int)entries.size()) selected_index = (int)entries.size() - 1;
                            draw_list();
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: {
                            int prev = find_next_track(selected_index, false);
                            if (prev != selected_index) {
                                selected_index = prev;
                                draw_list();
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
                            int next = find_next_track(selected_index, true);
                            if (next != selected_index) {
                                selected_index = next;
                                draw_list();
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_A: {
                            if (selected_index < 0 || selected_index >= (int)entries.size()) break;
                            Entry& sel = entries[selected_index];
                            if (sel.is_dir) {
                                current_path += (current_path == "/" ? "" : "/") + sel.name;
                                list_directory(current_path, true);
                                draw_list();
                            } else {
                                std::string filepath = current_path + "/" + sel.name;
                                if (read_metadata(filepath, current_meta))
                                    draw_playback(current_meta, 0);
                                launch_playgsf(filepath);
                                mode = MODE_PLAYBACK;
                                paused = false;
                                playback_start = clock_type::now();
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_B:
                            if (current_path != MUSIC_ROOT) {
                                size_t pos = current_path.find_last_of('/');
                                current_path = (pos == std::string::npos || current_path == MUSIC_ROOT) ? MUSIC_ROOT : current_path.substr(0, pos);
                                list_directory(current_path, true);
                                draw_list();
                            }
                            break;
                    }
                } else if (mode == MODE_PLAYBACK) {
                    switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_B:
                            kill_playgsf();
                            mode = MODE_LIST;
                            draw_list();
                            break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: {
                            int prev = find_next_track(selected_index, false);
                            if (prev != selected_index) {
                                selected_index = prev;
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta))
                                    draw_playback(current_meta, 0);
                                kill_playgsf();
                                launch_playgsf(filepath);
                                paused = false;
                                playback_start = clock_type::now();
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
                            int next = find_next_track(selected_index, true);
                            if (next != selected_index) {
                                selected_index = next;
                                std::string filepath = current_path + "/" + entries[selected_index].name;
                                if (read_metadata(filepath, current_meta))
                                    draw_playback(current_meta, 0);
                                kill_playgsf();
                                launch_playgsf(filepath);
                                paused = false;
                                playback_start = clock_type::now();
                            }
                            break;
                        }
                        case SDL_CONTROLLER_BUTTON_START:
                            if (playgsf_pid > 0) {
                                if (!paused) {
                                    kill(playgsf_pid, SIGSTOP);
                                    paused = true;
                                } else {
                                    kill(playgsf_pid, SIGCONT);
                                    paused = false;
                                }
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
