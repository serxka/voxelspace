#include <libndls.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CLAMP(x, a, b) ((x) > (b) ? (b) : (x) < (a) ? (a) : (x))

#define RSCALE (4)
#define S_WIDTH 320
#define S_HEIGHT 240
#define W_WIDTH (S_WIDTH/RSCALE)
#define W_HEIGHT (S_HEIGHT/RSCALE)

int mapsize;
uint8_t *greyscale_bmp;
uint8_t *level_bmp;
uint16_t *framebuffer[2];

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

static void die(const char *fmt, ...) {
	va_list args;
	va_start (args, fmt);
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, args);
	lcd_init(SCR_TYPE_INVALID);
	show_msgbox("OOPS", buf);
	va_end(args);
	exit(1);
}

void load_images(const char *basename, const char *ext) {
	char filename[256];
	int w, h, chans;

	snprintf(filename, sizeof(filename), "%s_bw.%s.tns", basename, ext);
	greyscale_bmp = stbi_load(filename, &w, &h, &chans, STBI_grey);
	if (greyscale_bmp == NULL || w != h || chans != 1)
		die("failed to load greyscale image './%s'", filename);

	int expected_size = w;

	snprintf(filename, sizeof(filename), "%s_col.%s.tns", basename, ext);
	level_bmp = stbi_load(filename, &w, &h, &chans, STBI_rgb);
	if (level_bmp == NULL || w != expected_size || h != expected_size || chans != 3)
		die("failed to load level image './%s'", filename);

	mapsize = w;
}

void init(void) {
	lcd_init(SCR_320x240_565);
	framebuffer[0] = malloc(S_HEIGHT * S_WIDTH * sizeof(uint16_t));
	framebuffer[1] = malloc(W_HEIGHT * W_WIDTH * sizeof(uint16_t));
	memset(framebuffer[1], 0, W_HEIGHT * W_WIDTH * sizeof(uint16_t));

	camera = (camera_t){
		.x = 512.0,
		.y = 512.0,
		.height = 40.0,
		.yaw = 0.0,
		.horizon = 40.0,
		.scale = 1.0,
		.distance = 80,
	};
}

static uint32_t sample(uint8_t *data, int size, int channels, int x, int y) {
	x /= 2;
	y /= 2;
	int idx = ((abs(y) % size) * size + (abs(x) % size)) * channels;
	if (channels == 1)
		return data[idx];
	else if (channels == 3)
		return data[idx + 0] << 16 | data[idx + 1] << 8 | data[idx + 2];
	else
		abort();
}

static void update(void) {
	// Camera orientation
	camera.yaw += (isKeyPressed(KEY_NSPIRE_4) + -1 * isKeyPressed(KEY_NSPIRE_6)) * 0.04;
	camera.horizon += (isKeyPressed(KEY_NSPIRE_PLUS) + -1 * isKeyPressed(KEY_NSPIRE_MINUS)) * 5.0;

	// Camera position
	double forward = isKeyPressed(KEY_NSPIRE_5) + -1 * isKeyPressed(KEY_NSPIRE_8);
	camera.x += forward * sin(camera.yaw) * camera.scale * 1.5;
	camera.y += forward * cos(camera.yaw) * camera.scale * 1.5;

	// Camera height
	camera.height += (isKeyPressed(KEY_NSPIRE_9) + -1.0 * isKeyPressed(KEY_NSPIRE_7)) * 0.8;
	uint8_t height_sample = sample(greyscale_bmp, mapsize, 1, (int)camera.x, (int)camera.y) + 5;
	if (height_sample > camera.height) // Collision with ground
		camera.height = height_sample;
}

static void draw_column(uint32_t x, uint32_t ybot, uint32_t ytop, uint32_t col) {
	if (ytop > ybot)
		return;

	uint16_t col4 = 0;
	col4 |= ((col >> 19) & 0x1F) << 11;
	col4 |= ((col >> 10) & 0x3F) <<  5;
	col4 |= ((col >>  3) & 0x1F) <<  0;

	ytop = ytop > W_HEIGHT ? W_HEIGHT : ytop;
	for (; ytop < ybot; ++ytop)
		framebuffer[1][ytop * W_WIDTH + x] = col4;
}

static void blit(void) {
	// copy over and scale
	for (uint32_t y = 0; y < W_HEIGHT; ++y) {
		for (uint32_t x = 0; x < W_WIDTH; ++x) {
			uint32_t xs = x * RSCALE;
			uint32_t ys = y * RSCALE;
			uint16_t col = framebuffer[1][y * W_WIDTH + x];
			for (uint32_t yl = 0; yl < RSCALE; ++yl)
				for (uint32_t xl = 0; xl < RSCALE; ++xl)
					framebuffer[0][(ys + yl) * S_WIDTH + (xs + xl)] = col;
		}
	}

	lcd_blit(framebuffer[0], SCR_320x240_565);
	for (uint32_t i = 0; i < W_WIDTH * W_HEIGHT; ++i)
		framebuffer[1][i] = 0x771F;
}

static void draw(void) {
	// Y-buffer
	uint16_t ybuf[W_WIDTH];
	for (uint32_t i = 0; i < W_WIDTH; ++i)
		ybuf[i] = W_HEIGHT;

	// Camera angles
	double sina = sin(camera.yaw);
	double cosa = cos(camera.yaw);

	// Draw from front to back. use y buffer
	for (uint32_t z = 1; z < camera.distance * 4; z += 4) {
		double zd = (double)z;

		// Left and right ends of our sampling line
		double splx = (-cosa*zd - sina*zd) * camera.scale;
		double sply = ( sina*zd - cosa*zd) * camera.scale;
		double sprx = ( cosa*zd - sina*zd) * camera.scale;
		double spry = (-sina*zd - cosa*zd) * camera.scale;

		// Segment the line into sample intervals
		double dx = (sprx - splx) / (double)W_WIDTH;
		double dy = (spry - sply) / (double)W_WIDTH;
		splx += camera.x;
		sply += camera.y;

		// Draw the columns left to right, starting from the top of the highest pixel
		double zs = (1.0 / zd * 240.0);
		for (uint32_t i = 0; i < W_WIDTH; ++i) {
			double height_sample = (double)sample(greyscale_bmp, mapsize, 1, (int)splx, (int)sply);
			uint16_t height_screen = (camera.height - height_sample) * zs + camera.horizon;
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
	for (uint32_t i = 0; i < 2; ++i)
		free(framebuffer[i]);
	lcd_init(SCR_TYPE_INVALID);
}

int main(int argc, char **argv) {
	enable_relative_paths(argv);
	(void)argc;
	
	// Process arguments
	const char *level = "res/level01";
	const char *ext = "png";

	// Setup program
	init();
	load_images(level, ext);

	// Main game loop
	bool quit = false;
	while (!quit) {
		quit = isKeyPressed(KEY_NSPIRE_ESC);
		update();
		draw();
		blit();
	}

	// Clean up resources
	cleanup();
	return 0;
}
