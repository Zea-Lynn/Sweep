#include "GLFW/glfw3.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <GLES3/gl3.h>
#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

typedef struct{
        float x, y, z;
} Vertex;

typedef struct{
        float x, y; 
} Vec2;

typedef struct{
        float u, v;
} UV;

#define DEFINE_SLICE(Type) typedef struct { uint64_t size; Type * data;} Type##_Slice;

DEFINE_SLICE(Vertex)

Vertex hex_grid_line_buffer[1<<10];

typedef enum{
        none = 0,
        hidden = 1 << 0,
        charged = 1 << 1,
        flagged = 1 << 3,
} Tile;


static int test_flags(int flags, int bits){
        return (flags & bits) == bits;
}

typedef struct{
        GLFWwindow * window;
        GLuint program;
        GLint ubo_location;
        GLint ubo_color_location;

        uint64_t frame;

        float line_width;

        Vertex hexagon_tile[7];
        float hexagon_diameter;
        int grid_width, grid_height;
        Vertex_Slice tile_points;
        Vertex_Slice hex_grid_lines;

        //sizeof width * height;
        Tile * tiles;
        uint8_t * mine_counts;

        //Live state
        int screen_width, screen_height;
        int total_mines;
        int won;
        enum {
                not_exploding,
                exploding,
                re_planting
        } failing;
        int exploding_mine_index;
        int clearing_row_index;
        int filling;
        int * tiles_to_search;
        int depth;
        int max_depth;

        int flag_button_was_released;
        int sweep_button_was_released;
}State;

State state_d;

typedef  char const * c_str;

static void crash(c_str message){
        fputs(message, stderr);
        abort();
}

typedef struct{
        int width, height, pixel_count, * pixels;
} hex_glyph;

#define ARRAY_SIZE(array) sizeof(array)/sizeof(array[0])

int a_glyph_pixels[] = {2, 3, 7, 9, 13, 15, 16, 18, 25, 28, 31, 33, 38, 39,};
hex_glyph a_glyph = {.width = 6, .height = 7, .pixel_count = ARRAY_SIZE(a_glyph_pixels), .pixels = a_glyph_pixels};

static GLuint load_shader(GLenum type, GLchar const * shader_src){
        puts("loading shader");
        GLuint shader = glCreateShader(type);
        if(!shader) crash("failed to create gl shader");

        //not passing a size could be a problem;
        glShaderSource(shader, 1, &shader_src, NULL);
        glCompileShader(shader);
        GLint compiled;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if(!compiled){
                GLint infolen = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infolen);
                if(infolen > 1){
                        char * infolog = malloc(infolen);
                        glGetShaderInfoLog(shader, infolen, NULL, infolog);
                        crash(infolog);
                }
        }
        return shader;
}

GLuint hexagon_indices[] = {
        0,1,2,
        2,3,0,
        0,3,4,
        4,5,0,
        0,5,6,
        6,1,0,
};

static void generate_hexagon(Vertex vertices[7], float width){
        vertices[0].x = 0;
        vertices[0].y = 0;
        vertices[0].z = 0;

        vertices[1].x = 0;
        vertices[1].y = width/2.0f;
        vertices[1].z = 0;

        float s = sinf(1.047198f);
        float c = cosf(1.047198f);

        for(int i = 0; i < 5; ++i){
                vertices[i+2].x = vertices[i+1].x * c - vertices[i+1].y * s;
                vertices[i+2].y = vertices[i+1].x * s + vertices[i+1].y * c;
                vertices[i+2].z = 0;
        }
}

static void settup(void * state_p){
        State * state = state_p;
        if(!glfwInit()) crash("no glfw.");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
        state->screen_width = 650;
        state->screen_height = 650;
        state->window = glfwCreateWindow(state->screen_width, state->screen_height, "sweep", NULL, NULL);

        glfwMakeContextCurrent(state->window);

        GLchar vShaderStr[] =  
                "#version 300 es\n"
                "in vec4 vPosition;\n"
                "out vec4 frag_color;\n"
                "uniform vec2 offset;\n"
                "uniform vec4 color;\n"
                "void main(){\n"
                "   gl_Position = vPosition + vec4(offset, 0, 0);\n"
                "   frag_color = color;\n"
                "}\n";
        
        GLchar fShaderStr[] =  
                "#version 300 es\n"
                "precision mediump float;\n"
                "in vec4 frag_color;\n"
                "out vec4 color;\n"
                "void main()\n"
                "{\n"
                "  color = vec4 ( 1.0, 1.0, 1.0, 1.0 ) * frag_color;\n"
                "}\n";

        GLuint vert = load_shader(GL_VERTEX_SHADER, vShaderStr);
        GLuint frag = load_shader(GL_FRAGMENT_SHADER, fShaderStr);
        state->program = glCreateProgram();
        if(!state->program) crash("failed to create program object");
        glAttachShader(state->program, vert);
        glAttachShader(state->program, frag);
        glBindAttribLocation(state->program, 0, "vPosition");
        glLinkProgram(state->program);
        GLint linked;
        glGetProgramiv(state->program, GL_LINK_STATUS, &linked);

        if(!linked){
                GLint infolen = 0;
                glGetProgramiv(state->program, GL_INFO_LOG_LENGTH, &infolen);
                if(infolen > 1){
                        char * infolog = malloc(infolen);
                        glGetShaderInfoLog(state->program, infolen, NULL, infolog);
                        crash(infolog);
                }
        }

        state->ubo_location = glGetUniformLocation(state->program, "offset");
        if(state->ubo_location < 0) crash("bad");

        state->ubo_color_location = glGetUniformLocation(state->program, "color");
        if(state->ubo_color_location < 0) crash("bad");
        

        glClearColor(.5,0,.5,1);

        // generate_hex_grid_lines(state);
        state->line_width = 1;

        state->grid_width = 11; 
        //TODO: calculate height from how many tiles can fit.
        state->grid_height = 12;
        state->hexagon_diameter = (2.0-(2.0/(state->grid_width * 0.866025404) * 0.5 ))/(state->grid_width * 0.866025404);



        generate_hexagon(state->hexagon_tile, state->hexagon_diameter);


        state->tiles = malloc(sizeof(Tile) * state->grid_width * state->grid_height);
        state->mine_counts = malloc(state->grid_width * state->grid_height); 
        for(int i = 0; i < state->grid_width * state->grid_height; ++i) state->tiles[i] = hidden; 
        for(int i = 0; i < state->grid_width * state->grid_height; ++i) state->mine_counts[i] = 0; 

        srand(42069);

        state->total_mines = state->grid_height * state->grid_width * 0.2;

        for(int i = 0; i < state->total_mines; ++i){
                int tile = rand() % (state->grid_width * state->grid_height);
                state->tiles[tile] |= charged;
        }

        state->max_depth = state->grid_width * state->grid_height;
        state->tiles_to_search = malloc(state->max_depth * sizeof(int));
#ifndef NDEBUG
        // state->show_charged = 1;
#endif
}

static Vec2 calculate_hexagon_offset(float hexagon_diameter, int x, int y){
        Vec2 offset = {0};
        double hex_width = hexagon_diameter * 0.866025404;
        double origin_offset = hex_width/2;
        if(y & 1) offset.x = (x * hexagon_diameter + hexagon_diameter/2) * 0.866025404 + origin_offset;
        else offset.x = x * hex_width + origin_offset;
        offset.y = y * hexagon_diameter * .75 + hexagon_diameter/2;
        return offset;
}

static void update(void * state_p){
        State * state = state_p;
        glfwPollEvents();

        double x_pos, y_pos;
        glfwGetCursorPos(state->window, &x_pos, &y_pos);
        y_pos = state->screen_height - y_pos;

        for(uint64_t i = 0; i < state->tile_points.size; ++i){
                //TODO: check radius of point to see if it overlaps with mouse
                //if two points both have the mouse pick the closest
                //if mouse right is pressed check tile
                //if mouse left is pressed plant flag
        }


        //TODO: move this somewhere else. most of this won't change that often.
        GLuint hexagon_object;
        glGenBuffers(1, &hexagon_object);
        glBindBuffer(GL_ARRAY_BUFFER, hexagon_object);
        glBufferData(GL_ARRAY_BUFFER, sizeof(state->hexagon_tile), state->hexagon_tile, GL_STATIC_DRAW);

        GLuint hex_grid_object;
        glGenBuffers(1, &hex_grid_object);
        glBindBuffer(GL_ARRAY_BUFFER, hex_grid_object);
        glBufferData(GL_ARRAY_BUFFER, state->hex_grid_lines.size * sizeof(Vertex), state->hex_grid_lines.data, GL_STATIC_DRAW);

        GLuint ubo;
        glGenBuffers(state->ubo_location, &ubo);

        glViewport(0, 0, state->screen_width, state->screen_height);

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(state->program);

        glBindBuffer(GL_ARRAY_BUFFER, hex_grid_object);
        glVertexAttribPointer(0, sizeof(Vertex)/sizeof(float), GL_FLOAT, 0,0, NULL);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_LINES, 0, state->hex_grid_lines.size);

        glBindBuffer(GL_ARRAY_BUFFER, hexagon_object);
        glVertexAttribPointer(0, sizeof(Vertex)/sizeof(float), GL_FLOAT, 0,0, NULL);
        glEnableVertexAttribArray(0);
        //TODO: maybe just use a circle radius to draw the tiles, this would make it so we could animate a transition from squares to ngons(maybe a new game mechanic).
        //      just use the fragment shader and pass it a bunch of points with a radius passed through the ubo.
        //      this would better match how we detect a click aswell.
        //      although that would mean we are stuck to 2d until I create an algorithm that can create mesh to mach the ngons.

        int hoverd_x = -1, hoverd_y = -1;
        for(int x = 0; x < state->grid_width; ++x){
                for(int y = 0; y < state->grid_height; ++y){
                        Vec2 offset = calculate_hexagon_offset(state->hexagon_diameter, x, y);

                        float mouse_distance = sqrt(pow(x_pos - offset.x * state->screen_width * 0.5, 2) + pow(y_pos - offset.y * state->screen_height * 0.5,2));
                        if(mouse_distance <= (state->hexagon_diameter * 0.5) * state->screen_width * 0.5){
                                if(hoverd_x > -1 || hoverd_y > -1){
                                        Vec2 other_offset = calculate_hexagon_offset(state->hexagon_diameter, hoverd_x, hoverd_y);
                                        float other_mouse_distance = sqrt(pow(x_pos - other_offset.x * state->screen_width * 0.5, 2) + pow(y_pos - other_offset.y * state->screen_height * 0.5,2));
                                        if(other_mouse_distance > mouse_distance){
                                                hoverd_x = x;
                                                hoverd_y = y;
                                        }
                                }else{
                                        hoverd_x = x;
                                        hoverd_y = y;
                                }
                        }
                }
        }

        if(glfwGetKey(state->window, GLFW_KEY_R) == GLFW_PRESS){
                state->failing = re_planting;
        }

        if(state->failing == exploding){
                int max_tiles = state->grid_width * state->grid_height;
                if(state->exploding_mine_index == max_tiles){
                        state->exploding_mine_index = 0;
                        state->failing = re_planting;
                }else for(;state->exploding_mine_index < max_tiles; ++state->exploding_mine_index){
                        if(test_flags(state->tiles[state->exploding_mine_index], charged)){
                                state->tiles[state->exploding_mine_index] &= ~hidden;
                        } else continue;
                        ++state->exploding_mine_index;
                        break;
                }
        }else if(state->failing == re_planting){
                if(state->clearing_row_index < state->grid_height){
                        for(int x = 0; x < state->grid_width; ++x){
                                state->mine_counts[state->clearing_row_index * state->grid_width + x] = 0;
                                state->tiles[state->clearing_row_index * state->grid_width + x] = hidden;
                        }
                        ++state->clearing_row_index;
                }else if(state->clearing_row_index == state->grid_height){
                        for(int i = 0; i < state->total_mines; ++i){
                                int tile = rand() % (state->grid_width * state->grid_height);
                                state->tiles[tile] |= charged;
                        }
                        state->clearing_row_index = 0;
                        state->failing = not_exploding;
                        state->won = 0;
                }

        }else if(state->filling && state->depth > -1){
                while(1){
                        if(state->depth >= state->max_depth) crash("somtin wrong");
                        
                        int x = state->tiles_to_search[state->depth] % state->grid_width;
                        int y = state->tiles_to_search[state->depth] / state->grid_width;

                        int at_max_height = y == state->grid_height-1;
                        int at_min_height = y==0;

                        int offset_left_x = (x-!(y&1));
                        int offset_right_x = (x+(y&1));
                        int left_x = (x-1);
                        int right_x = (x+1);

                        Tile top_left = none;
                        Tile top_right = none;
                        if(!at_max_height){
                                if(offset_left_x >= 0) top_left = state->tiles[(y+1) * state->grid_width + offset_left_x];
                                if(offset_right_x < state->grid_width) top_right = state->tiles[(y+1) * state->grid_width + offset_right_x];
                        }

                        Tile bottom_left = none;
                        Tile bottom_right = none;
                        if(!at_min_height){
                                if(offset_left_x >= 0) bottom_left = state->tiles[(y-1) * state->grid_width + (x-!(y&1))];
                                if(offset_right_x < state->grid_width) bottom_right = state->tiles[(y-1) * state->grid_width + (x+(y&1))];
                        }

                        Tile left = none;
                        Tile right = none;
                        {
                                if(left_x >= 0) left = state->tiles[y * state->grid_width + left_x];
                                if(right_x < state->grid_width) right = state->tiles[y * state->grid_width + right_x];

                        }

                        uint8_t mines_found = 0;
                        if(top_left != none && (top_left & charged) == charged) ++mines_found;
                        if(top_right != none && (top_right & charged) == charged) ++mines_found;
                        if(bottom_left != none && (bottom_left & charged) == charged) ++mines_found;
                        if(bottom_right != none && (bottom_right & charged) == charged) ++mines_found;
                        if(left != none && (left & charged) == charged) ++mines_found;
                        if(right != none && (right & charged) == charged) ++mines_found;

                        state->tiles[state->tiles_to_search[state->depth]] &= ~hidden;

                        if(mines_found > 0){
                                state->mine_counts[state->tiles_to_search[state->depth]] = mines_found;
                                if(state->depth == 0){
                                        state->filling = 0;
                                        break;
                                }
                                else --state->depth;
                        }else{
                                if(test_flags(left, hidden) && left_x >= 0){
                                        state->tiles_to_search[++state->depth] = y * state->grid_width + left_x;
                                        break;
                                }else if(test_flags(right, hidden) && right_x < state->grid_width){
                                        state->tiles_to_search[++state->depth] = y * state->grid_width + right_x;
                                        break;
                                }else if(test_flags(top_left, hidden) && !at_max_height && offset_left_x >= 0){
                                        state->tiles_to_search[++state->depth] = (y+1) * state->grid_width + offset_left_x;
                                        break;
                                }else if(test_flags(top_right, hidden) && !at_max_height && offset_right_x < state->grid_width){
                                        state->tiles_to_search[++state->depth] = (y+1) * state->grid_width + offset_right_x;
                                        break;
                                }else if(test_flags(bottom_left, hidden) && !at_min_height && offset_left_x >= 0){
                                        state->tiles_to_search[++state->depth] = (y-1) * state->grid_width + offset_left_x;
                                        break;
                                }else if(test_flags(bottom_right, hidden) && !at_min_height && offset_right_x < state->grid_width){
                                        state->tiles_to_search[++state->depth] = (y-1) * state->grid_width + offset_right_x;
                                        break;
                                }else{
                                        if(state->depth <= 0){
                                                state->filling = 0;
                                                break;
                                        } else --state->depth;
                                }
                        }
                };
        }else{
                state->filling = 0;
                state->depth = 0;
        }

        state->won = 1;
        for(Tile * tile = state->tiles; tile != state->tiles + (state->grid_width * state->grid_height); ++tile){
                if(test_flags(*tile, charged) && !test_flags(*tile, flagged)){
                        state->won = 0;
                } else if(test_flags(*tile, hidden) && !test_flags(*tile, charged)){
                        state->won = 0;
                }
        }

        //Rendering and input.
        int sweep_button_pressed = glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        int flag_button_pressed = glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

        //TODO: use glDrawElementsInstaced if I can somehow get that to work on the web.
        for(int x = 0; x < state->grid_width; ++x){
                for(int y = 0; y < state->grid_height; ++y){
                        int tile_index = y * state->grid_width + x;
                        Tile * tile = &state->tiles[tile_index];

                        Vec2 offset = calculate_hexagon_offset(state->hexagon_diameter, x, y);
                        glUniform2f(state->ubo_location, offset.x-1, offset.y-1);

                        // state->ubo.g = (float)(state->frame & UINT8_MAX)/UINT8_MAX;
                        float r = 1 - ((float)x/(float)state->grid_width);
                        float b = 1 - ((float)y/(float)state->grid_height);
                        float g = 0;
                        if(x == hoverd_x && y == hoverd_y){
                                if(state->filling){
                                }else if(state->sweep_button_was_released && sweep_button_pressed){
                                        state->depth = 0;
                                        state->tiles_to_search[state->depth] = tile_index;
                                        if(test_flags(*tile, charged) && !test_flags(*tile, flagged)){
                                                state->failing = exploding;
                                                // printf("whatever cell:%d\n", tile);
                                        }else if(state->tiles[tile_index] == hidden){
                                                state->tiles_to_search[state->depth] = tile_index;
                                                state->filling = 1;
                                        }
                                }else if (state->flag_button_was_released && flag_button_pressed){
                                        if(test_flags(*tile, flagged)) *tile &= ~flagged;
                                        else *tile |= flagged;
                                } 
                                g = 1;
                        }else g = 0;

                        if(state->tiles_to_search[state->depth] == tile_index){
                                r = 1;
                        }

                        if(!test_flags(state->tiles[tile_index], hidden)){
                                if(test_flags(state->tiles[tile_index], charged)){
                                        r = 1;
                                        g = 0;
                                        b = 0;
                                }else{
                                        if(state->won){
                                                r = .2;
                                                g = .2 + ((float)state->mine_counts[tile_index]/6) * .8 ;
                                                b = .2;
                                        }else{
                                                r = .2 + ((float)state->mine_counts[tile_index]/6) * .8 ;
                                                g = .2;
                                                b = .2;
                                        }
                                }
                        }else if(test_flags(state->tiles[tile_index], flagged)){
                                r = .0;
                                g = .5;
                                b = .5;
                        }

                        glUniform4f(state->ubo_color_location, r, g, b, 1);

                        glDrawElements(GL_TRIANGLES, 18, GL_UNSIGNED_INT, hexagon_indices);
                }
        }

        state->sweep_button_was_released = glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_RELEASE;
        state->flag_button_was_released = glfwGetMouseButton(state->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE;

        glfwSwapBuffers(state->window);
        ++state->frame;
}

int main(void){
        puts("initalizing");
        settup(&state_d);
#if defined(EMSCRIPTEN)
        emscripten_set_main_loop_arg(update, &state_d, 0, 0);
#else
        while(!glfwWindowShouldClose(state_d.window)) update(&state_d);
#endif
}
