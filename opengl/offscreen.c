/*
http://stackoverflow.com/questions/3191978/how-to-use-glut-opengl-to-render-to-a-file/14324292#14324292
*/

#define LIBPNG 0
#define FFMPEG 1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glut.h>
#include <GL/glext.h>

#if LIBPNG
#include <png.h>
#endif

#if FFMPEG
#include <libavutil/opt.h> /* av_opt_set */
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h> /* av_image_alloc */
#endif

enum Constants { SCREENSHOT_MAX_FILENAME = 256 };
static GLuint fbo;
static GLuint rbo_color;
static GLuint rbo_depth;
static const GLenum FORMAT = GL_RGBA;
static const GLuint format_nchannels = 4;
static const unsigned int HEIGHT = 100;
static const unsigned int WIDTH = 100;
static int offscreen = 1;
static unsigned int max_nframes = 100;
static unsigned int nframes = 0;
static unsigned int time0;

/* Model. */
static double angle;
static double delta_angle;

/*
Take screenshot with glReadPixels and save to a file in PPM format.
-   filename: file path to save to, without extension
-   width: screen width in pixels
-   height: screen height in pixels
-   format: glReadPixelsFormat
-   format_nchannels: number of channels per pixel (e.g. R, G, B, A)
    This is implied by format, but we haven't found a built-in way to get this
    information automatically without hard-coding a large switch.
-   pixels: intermediate buffer to avoid repeated mallocs across multiple calls.
    Contents of this buffer do not matter. May be NULL, in which case it is initialized.
    You must `free` it when you won't be calling this function anymore.
*/
static GLubyte *pixels = NULL;
static void screenshot_ppm(const char *filename, unsigned int width, unsigned int height,
         GLenum format, unsigned int format_nchannels, GLubyte **pixels) {
    size_t i, j, k, cur;
    FILE *f = fopen(filename, "w");
    fprintf(f, "P3\n%d %d\n%d\n", width, height, 255);
    *pixels = realloc(*pixels, format_nchannels * sizeof(GLubyte) * width * height);
    glReadPixels(0, 0, width, height, format, GL_UNSIGNED_BYTE, *pixels);
    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++) {
            cur = format_nchannels * ((height - i - 1) * width + j);
            fprintf(f, "%3d %3d %3d ", (*pixels)[cur], (*pixels)[cur + 1], (*pixels)[cur + 2]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

#if LIBPNG
static png_byte *png_bytes = NULL;
static png_byte **png_rows = NULL;
static void screenshot_png(const char *filename, unsigned int width,
        unsigned int height, GLenum format, unsigned int format_nchannels,
        GLubyte **pixels, png_byte **png_bytes, png_byte ***png_rows) {
    size_t i, j, cur, cur_png, nvals;
    FILE *f = fopen(filename, "wb");
    nvals = format_nchannels * width * height;
    *pixels = realloc(*pixels, nvals * sizeof(GLubyte));
    *png_bytes = realloc(*png_bytes, nvals * sizeof(png_byte));
    *png_rows = realloc(*png_rows, height * sizeof(png_byte*));
    glReadPixels(0, 0, width, height, format, GL_UNSIGNED_BYTE, *pixels);
    for (i = 0; i < nvals; i++)
        (*png_bytes)[i] = (*pixels)[i];
    for (i = 0; i < height; i++)
        (*png_rows)[height - i - 1] = &(*png_bytes)[i * width * format_nchannels];
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) abort();
    png_infop info = png_create_info_struct(png);
    if (!info) abort();
    if (setjmp(png_jmpbuf(png))) abort();
    png_init_io(png, f);
    png_set_IHDR(
        png,
        info,
        width,
        height,
        8,
        PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );
    png_write_info(png, info);
    png_write_image(png, *png_rows);
    png_write_end(png, NULL);
    fclose(f);
}
#endif

#if FFMPEG
static AVCodecContext *c = NULL;
static AVFrame *frame;
static FILE *f;
int64_t pts = 0;

static void screenshot_mpg_init(const char *filename, int width, int height) {
    AVCodec *codec;
    int codec_id = AV_CODEC_ID_MPEG1VIDEO;
    int ret;

    avcodec_register_all();
    codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }
    c->bit_rate = 400000;
    c->width = width;
    c->height = height;
    c->time_base = (AVRational){1, 25};
    c->gop_size = 10;
    c->max_b_frames = 1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (codec_id == AV_CODEC_ID_H264)
        av_opt_set(c->priv_data, "preset", "slow", 0);
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }
    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;
    ret = av_image_alloc(frame->data, frame->linesize, c->width, c->height, c->pix_fmt, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate raw picture buffer\n");
        exit(1);
    }
}

static void screenshot_mpg_deinit(void) {
    AVPacket pkt;
    int ret, x, y, got_output;
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    for (got_output = 1; got_output; pts++) {
        fflush(stdout);
        ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
        if (ret < 0) {
            fprintf(stderr, "Error encoding frame\n");
            exit(1);
        }
        if (got_output) {
            fwrite(pkt.data, 1, pkt.size, f);
            av_packet_unref(&pkt);
        }
    }
    fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);
    avcodec_close(c);
    av_free(c);
    av_freep(&frame->data[0]);
    av_frame_free(&frame);
}

/*
Compress one frame.
screenshot_mpg_init must have been called previously,
and screenshot_mpg_deinit must be called to clean this afterwards.
Adapted from https://github.com/FFmpeg/FFmpeg/blob/4b150fbe1f3905f8245f63d74ff72f2ef92d9717/doc/examples/decoding_encoding.c
*/
static void screenshot_mpg(unsigned int width, unsigned int height, GLenum format,
        unsigned int format_nchannels, GLubyte **pixels) {
    AVPacket pkt;
    int ret, x, y, got_output;
    size_t nvals;

    nvals = format_nchannels * width * height;
    *pixels = realloc(*pixels, nvals * sizeof(GLubyte));
    glReadPixels(0, 0, width, height, format, GL_UNSIGNED_BYTE, *pixels);

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    frame->pts = pts;
    for (y = 0; y < c->height; y++) {
        for (x = 0; x < c->width; x++) {
            frame->data[0][y * frame->linesize[0] + x] = x + y + pts * 3;
        }
    }
    for (y = 0; y < c->height / 2; y++) {
        for (x = 0; x < c->width / 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = 128 + y + pts * 2;
            frame->data[2][y * frame->linesize[2] + x] =  64 + x + pts * 5;
        }
    }
    ret = avcodec_encode_video2(c, &pkt, frame, &got_output);
    if (ret < 0) {
        fprintf(stderr, "Error encoding frame\n");
        exit(1);
    }
    if (got_output) {
        fwrite(pkt.data, 1, pkt.size, f);
        av_packet_unref(&pkt);
    }
    pts++;
}
#endif

static int model_init(void) {
    angle = 0;
    delta_angle = 1;
}

static int model_update(void) {
    angle += delta_angle;
    return 0;
}

static int model_finished(void) {
    return nframes >= max_nframes;
}

static void init(void)  {
    int glget;

    if (offscreen) {
        /*  Framebuffer */
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        /* Color renderbuffer. */
        glGenRenderbuffers(1, &rbo_color);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_color);
        /* Storage must be one of: */
        /* GL_RGBA4, GL_RGB565, GL_RGB5_A1, GL_DEPTH_COMPONENT16, GL_STENCIL_INDEX8. */
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, WIDTH, HEIGHT);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo_color);

        /* Depth renderbuffer. */
        glGenRenderbuffers(1, &rbo_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, WIDTH, HEIGHT);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo_depth);

        glReadBuffer(GL_COLOR_ATTACHMENT0);

        /* Sanity check. */
        assert(glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &glget);
        assert(WIDTH * HEIGHT < (unsigned int)glget);
    } else {
        glReadBuffer(GL_BACK);
    }

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glEnable(GL_DEPTH_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glViewport(0, 0, WIDTH, HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

    time0 = glutGet(GLUT_ELAPSED_TIME);
    model_init();
#if FFMPEG
    screenshot_mpg_init("tmp.mpg", WIDTH, HEIGHT);
#endif
}

static void deinit(void)  {
    printf("FPS = %f\n", 1000.0 * nframes / (double)(glutGet(GLUT_ELAPSED_TIME) - time0));
    free(pixels);
#if LIBPNG
    free(png_bytes);
    free(png_rows);
#endif
#if FFMPEG
    screenshot_mpg_deinit();
#endif
    if (offscreen) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &rbo_color);
        glDeleteRenderbuffers(1, &rbo_depth);
    }
}

static void draw_scene(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glRotatef(angle, 0.0f, 0.0f, -1.0f);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glEnd();
}

static void display(void) {
    char extension[SCREENSHOT_MAX_FILENAME];
    char filename[SCREENSHOT_MAX_FILENAME];
    draw_scene();
    if (offscreen) {
        glFlush();
    } else {
        glutSwapBuffers();
    }
#if LIBPNG
    strcpy(extension, ".png");
#else
    strcpy(extension, ".ppm");
#endif
    snprintf(filename, SCREENSHOT_MAX_FILENAME, "tmp%d%s", nframes, extension);
#if LIBPNG
    screenshot_png(filename, WIDTH, HEIGHT, FORMAT, format_nchannels, &pixels, &png_bytes, &png_rows);
#else
# if FFMPEG
    screenshot_mpg(WIDTH, HEIGHT, FORMAT, format_nchannels, &pixels);
# else
    screenshot_ppm(filename, WIDTH, HEIGHT, FORMAT, format_nchannels, &pixels);
# endif
#endif
    nframes++;
    if (model_finished())
        exit(EXIT_SUCCESS);
}

static void idle(void) {
    while (model_update());
    glutPostRedisplay();
}

int main(int argc, char **argv) {
    GLint glut_display;
    glutInit(&argc, argv);
    if (argc > 1)
        offscreen = 0;
    if (offscreen) {
        /* TODO: if we use anything smaller than the window, it only renders a smaller version of things. */
        /*glutInitWindowSize(50, 50);*/
        glutInitWindowSize(WIDTH, HEIGHT);
        glut_display = GLUT_SINGLE;
    } else {
        glutInitWindowSize(WIDTH, HEIGHT);
        glutInitWindowPosition(100, 100);
        glut_display = GLUT_DOUBLE;
    }
    glutInitDisplayMode(glut_display | GLUT_RGBA | GLUT_DEPTH);
    glutCreateWindow(argv[0]);
    if (offscreen) {
        /* TODO: if we hide the window the program blocks. */
        /*glutHideWindow();*/
    }
    init();
    glutDisplayFunc(display);
    glutIdleFunc(idle);
    atexit(deinit);
    glutMainLoop();
    return EXIT_SUCCESS;
}