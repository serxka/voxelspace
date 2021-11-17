#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <SDL2/SDL.h>

#define FRAMETIME (16.666)
#define CLAMP(x, a, b) ((x) > (b) ? (b) : (x) < (a) ? (a) : (x))

SDL_Window *win;
SDL_Renderer *renderer;
int win_w;
int win_h;

size_t mapsize;
uint8_t *greyscale_bmp;
uint8_t *level_bmp;

typedef struct {
	double x;
	double y;
	double height;
	double yaw;
	double horizon;
	double scale;
	uint32_t distance;
}camera_t;

camera_t camera;

double val = 240.0;

static void die(const char *fmt, ...) {
	va_list args;
	va_start (args, fmt);
	fprintf(stderr, "[OOPS] ");
	vfprintf(stderr, fmt, args);
	putc('\n', stderr);
	va_end(args);
	exit(1);
}

void load_images(const char *basename, const char *ext) {
	char filename[256];
	int w, h, chans;
	
	snprintf(filename, sizeof(filename), "%s_bw.%s", basename, ext);
	greyscale_bmp = stbi_load(filename, &w, &h, &chans, STBI_grey);
	if (greyscale_bmp == NULL || w != h || chans != 1)
		die("failed to load greyscale image './%s'", filename);
	
	int expected_size = w;

	snprintf(filename, sizeof(filename), "%s_col.%s", basename, ext);
	level_bmp = stbi_load(filename, &w, &h, &chans, STBI_rgb);
	if (level_bmp == NULL || w != expected_size || h != expected_size || chans != 3)
		die("failed to load level image './%s'", filename);
	
	mapsize = w;
}

void init(void) {
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		die("failed to init sdl %s", SDL_GetError());

	win = SDL_CreateWindow("VoxelSpace", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600, 0);
	if (win == NULL)
		die("failed to create window: %s", SDL_GetError());
	renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (renderer == NULL)
		die("failed to create renderer: %s", SDL_GetError());

	camera = (camera_t){
		.x = 512.0,
		.y = 512.0,
		.height = 80.0,
		.yaw = 0.0,
		.horizon = 100.0,
		.scale = 1.0,
		.distance = 800,
	};
}

static uint32_t sample(uint8_t *data, size_t size, uint8_t channels, int x, int y) {
	int idx = ((abs(y) % size) * size + (abs(x) % size)) * channels;
	if (channels == 1)
		return data[idx];
	else if (channels == 3)
		return data[idx + 0] << 16 | data[idx + 1] << 8 | data[idx + 2];
}

void update(void) {
	// SDL_GetRendererOutputSize(renderer, &win_w, &win_h);
	win_w = 800;
	win_h = 600;
	const uint8_t *key_states = SDL_GetKeyboardState(NULL);
	
	// View distance
	camera.distance += key_states[SDL_SCANCODE_LEFTBRACKET] * -20 + key_states[SDL_SCANCODE_RIGHTBRACKET] * 20;
	camera.distance = CLAMP(camera.distance, 40, 3000);
	
	// Camera orientation
	camera.yaw += (key_states[SDL_SCANCODE_LEFT] + -1 * key_states[SDL_SCANCODE_RIGHT]) * 0.04;
	camera.yaw += (key_states[SDL_SCANCODE_A] + -1 * key_states[SDL_SCANCODE_D]) * 0.04;
	camera.horizon += (key_states[SDL_SCANCODE_UP] + -1 * key_states[SDL_SCANCODE_DOWN]) * 10.0;
	
	// Camera position
	double forward = key_states[SDL_SCANCODE_S] + -1 * key_states[SDL_SCANCODE_W];
	camera.x += forward * sin(camera.yaw) * camera.scale;
	camera.y += forward * cos(camera.yaw) * camera.scale;
	
	val += (key_states[SDL_SCANCODE_T] + -1 * key_states[SDL_SCANCODE_G]) * 1.0;

	// Camera height
	camera.height += (key_states[SDL_SCANCODE_E] + -1.0 * key_states[SDL_SCANCODE_Q]) * 2.0;
	uint8_t height_sample = sample(greyscale_bmp, mapsize, 1, (int)camera.x, (int)camera.y) + 10;
	if (height_sample > camera.height) // Collision with ground
		camera.height = height_sample;
}

static void draw_column(int x, uint16_t ybot, uint16_t ytop, uint32_t col) {
	if (ytop > ybot)
		return;
	SDL_SetRenderDrawColor(renderer, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, SDL_ALPHA_OPAQUE);
	SDL_RenderDrawLine(renderer, x, ybot, x, CLAMP(ytop, 0, win_h));
}

// Uses a disgusting amount of floating point math, ideally should use none
// TODO: add LOD, render less often the further away
void draw(void) {
	// Y-buffer
	uint16_t ybuf[win_w];
	for (int i = 0; i < win_w; ++i)
		ybuf[i] = win_h;
	
	// Camera angles
	double sina = sin(camera.yaw);
	double cosa = cos(camera.yaw);
	
	// Draw from front to back. use y buffer
	for (int z = 1; z < camera.distance; z += 1) {
		// Left and right ends of our sampling line
		double splx = (-cosa*z - sina*z) * camera.scale;
		double sply = ( sina*z - cosa*z) * camera.scale;
		double sprx = ( cosa*z - sina*z) * camera.scale;
		double spry = (-sina*z - cosa*z) * camera.scale;
		
		// Segment the line into sample intervals
		double dx = (sprx - splx) / (double)win_w;
		double dy = (spry - sply) / (double)win_w;
		splx += camera.x;
		sply += camera.y;
		
		// Draw the columns left to right, starting from the top of the highest pixel
		for (int i = 0; i < win_w; ++i) {
			uint8_t height_sample = sample(greyscale_bmp, mapsize, 1, (int)splx, (int)sply);
			uint16_t height_screen = (camera.height - (double)height_sample) * (1.0 / (double)z * val) + camera.horizon;
			uint32_t diffuse_sample = sample(level_bmp, mapsize, 3, (int)splx, (int)sply);
			
			draw_column(i, ybuf[i], height_screen, diffuse_sample);
			if (height_screen < ybuf[i])
				ybuf[i] = height_screen;
			
			splx += dx;
			sply += dy;
		}
	}
}

void cleanup(void) {
	// Free level resources
	free(greyscale_bmp);
	free(level_bmp);

	// Free SDL resources
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(win);
	SDL_Quit();
}

void print_usage(char *prog_name) {
	fprintf(stderr,
		"Usage: %s [OPTIONS]...\n"
		"Render a height map with colour.\n\n", prog_name);
	fprintf(stderr,
		"Optional Arguments:\n"
		"  -h             displays this prompt\n"
		"  -l [level]     takes a level prefix,\n"
		"                   e.g. -l 'res/level02'\n"
		"  -e [extension] specifies an extension for the level\n"
		"  -s [scale]     takes a number as a scale for size and movement\n\n");
	fprintf(stderr,
		"Source available at: <https://github.com/serxka/voxelspace>\n");
}

int main(int argc, char *argv[]) {
	// Process arguments
	int c;
	char *level = "res/level02";
	char *ext = "png";
	double scale = 1.0;
	while ((c = getopt(argc, argv, "hs:l:e:")) != -1)
		switch (c) {
			case 'l':
				level = optarg;
				break;
			case 's':
				scale = atof(optarg);
				break;
			case 'e':
				ext = optarg;
				break;
			case 'h':
				print_usage(argv[0]);
				exit(1);
			case '?':
				die("opt -%c requires an argument", optopt);
		}
		
	// Setup SDL
	init();
	camera.scale = scale;

	// Load resources, like level data
	load_images(level, ext);

	bool quit = false;
	while (!quit) {
		int start_tick = SDL_GetTicks();
		// Poll for events
		SDL_Event evt;
		while (SDL_PollEvent(&evt) != 0)
			switch (evt.type) {
				case SDL_QUIT:
					quit = true;
					break;
				default:
					break;
			}
		// Update state
		update();
		
		// Render our level
		SDL_SetRenderDrawColor(renderer, 135, 206, 235, SDL_ALPHA_OPAQUE);
		SDL_RenderClear(renderer);
		// clock_t start_time = clock();
		draw();
		// double elapsed_time = (double)(clock() - start_time) / CLOCKS_PER_SEC;
		// printf("drawing took %fms\n", elapsed_time * 1000.0);
		SDL_RenderPresent(renderer);
		
		// Wait for next frame
		int current_tick = SDL_GetTicks();
		int period = FRAMETIME - (current_tick - start_tick);
		period = period < 0 ? 0 : period;
		SDL_Delay(period);
	}
	// Clean up resources
	cleanup();
	return 0;
}
