#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <termios.h>
#include <gmodule.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static struct termios new_t, old_t;

/* #define DEBUG 1 */
static gchar *opt_pipefile = NULL;
const char* pipefile = NULL;

#ifdef DEBUG
#define PRINT(...) g_print(__VA_ARGS__)
#else
#define PRINT(...)
#endif

#define DEFAULT_EFFECTS "identity,exclusion,navigationtest," \
	"agingtv,videoflip,vertigotv,gaussianblur,shagadelictv,edgetv"

typedef struct {
	GstElement *src, *adder;
	GstElement *conv, *resample, *caps;
	GstPad *src_pad;
	gint free;
	gint playing;
	int id;
} PipeData;

const char* BOARD[] = {
	"/home/haha/sounds/2.wav",
	"/home/haha/sounds/1.wav",
	"/home/haha/1.wav",
	"/home/haha/2.wav",
};

static GstElement *adder;
static GstElement *conv;
static GstElement *pipeline;
static GMainLoop *loop;
static int pipecount = 0;

static GAsyncQueue *free_pipes = NULL;
static GList *pipes = NULL;
static void pad_added_handler (GstElement *src, GstPad *pad, void *data);
static void drained_handler (GstElement *src, GstPad *pad, void *data);
PipeData* make_pipe(const gchar *file);
int create_pid(const char *progName, const char *pidFile, int flags);
int is_playing();

void set_shadow()                                                                                                                                                                                                                         
{                                                                                                                                                                                                                                        
	tcgetattr(0, &old_t);                                                                                                                                                                                                                
	new_t = old_t;                                                                                                                                                                                                                       
	new_t.c_lflag &= (~ICANON);                                                                                                                                                                                                           
	new_t.c_lflag &= (~ECHO);                                                                                                                                                                                                            
	new_t.c_cc[VTIME] = 0;                                                                                                                                                                                                               
	new_t.c_cc[VMIN] = 1;                                                                                                                                                                                                                
	tcsetattr(0, TCSANOW, &new_t);                                                                                                                                                                                                       
}                                                                                                                                                                                                                                        

static GstPadProbeReturn
block_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	PipeData *pipe = (PipeData*) user_data;
	if(!pipe->free) {
		PRINT("blocking p%d\n", pipe->id);
		return GST_PAD_PROBE_OK;
	}
	PRINT("unblocking p%d\n", pipe->id);
	return GST_PAD_PROBE_REMOVE;
}

static void
setnull(GstElement *element, gpointer data) {
	PipeData *pipe = (PipeData*) data;
	gst_element_set_state(element, GST_STATE_NULL);
	gst_bin_remove(GST_BIN(pipeline), pipe->src);
	pipe->src = NULL;
	g_atomic_int_set(&(pipe->free), 1);
	g_async_queue_push(free_pipes, pipe);
	PRINT("pipe %d removed.\n", pipe->id);
}
static int remove_src(gpointer data) {
	PipeData *pipe = (PipeData*) data;
	PRINT("removing src, pipe %d.\n", pipe->id);

	if(g_atomic_int_get(&(pipe->free)) == 1) {
		g_print("pipe %d freed already!\n", pipe->id);
		return FALSE;
	}
	GstPad *caps_pad = gst_element_get_static_pad(pipe->caps, "src");
	GstPad *sink_pad = gst_pad_get_peer(caps_pad);
	if(sink_pad) {
		g_assert(sink_pad);
		gst_pad_unlink(caps_pad, sink_pad);
		gst_element_release_request_pad(adder, sink_pad);
		gst_object_unref(sink_pad);
	}

	gst_object_unref(caps_pad);
	gst_element_call_async(pipe->src, setnull, pipe, NULL);
	/* gst_element_set_state(pipe->src, GST_STATE_NULL); */
	/* g_print("removing src from bin..\n"); */
	/* gst_bin_remove(GST_BIN(pipeline), pipe->src); */
	/* pipe->src = NULL; */
	/* g_print("set free\n"); */
	/* g_atomic_int_set(&(pipe->free), 1); */
	/* g_async_queue_push(free_pipes, pipe); */
	/* g_print("pipe %d removed.\n", pipe->id); */
	return FALSE;
}


static GstPadProbeReturn
event_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
	const char *name = GST_EVENT_TYPE_NAME(GST_PAD_PROBE_INFO_DATA (info));
	GstEventType ev = GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info));
	PipeData *pipe = (PipeData*) user_data;
	PRINT("p%d e: %s\n", pipe->id, name);
	if(ev == GST_EVENT_STREAM_START) {
		g_atomic_int_set(&(pipe->playing), 1);
		gst_element_set_state(pipeline, GST_STATE_PLAYING);
	} else if(ev == GST_EVENT_EOS) {
		/* g_print("Got EOS, freeing pipe\n"); */
		g_atomic_int_set(&(pipe->playing), 0);
		remove_src(pipe);
		/* gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BLOCKING, block_cb, user_data, NULL); */
		/* gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID (info)); */
		/* g_idle_add(remove_src, pipe); */
	}
	return GST_PAD_PROBE_PASS;
}

static gboolean
bus_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
	GMainLoop *loop = user_data;
	switch (GST_MESSAGE_TYPE (msg)) {
		case GST_MESSAGE_ERROR: {
			GError *err = NULL;
			gchar *dbg;

			gst_message_parse_error (msg, &err, &dbg);
			gst_object_default_error (msg->src, err, dbg);
			g_clear_error (&err);
			g_free (dbg);
			g_main_loop_quit (loop);
			break;
		}
		case GST_MESSAGE_STATE_CHANGED: {
			/* if (GST_MESSAGE_SRC (msg) != GST_OBJECT (pipeline)) { */
			/* 	break; */
			/* } */
			GstState old_state, new_state;
			gst_message_parse_state_changed (msg, &old_state, &new_state, NULL);
			PRINT ("Element %s changed state from %s to %s.\n",
					GST_OBJECT_NAME (msg->src),
					gst_element_state_get_name (old_state),
					gst_element_state_get_name (new_state));
			break;
		}
		case GST_MESSAGE_APPLICATION: {
			if(gst_message_has_name (message, "ready-to-go")) {
				PRINT("all ready");
				gst_element_set_state(pipeline, GST_STATE_PLAYING);
			}
			break;
		}
		case GST_MESSAGE_EOS: {
			/* g_main_loop_quit (loop); */
			break;
		}
		default:
		   break;
	}
	return TRUE;
}

int is_playing() {
	GList *l;
	int playing = 0;
	for(l = pipes; l != NULL; l = l->next) {
		PipeData *pipe = (PipeData*)l->data;
		if(g_atomic_int_get(&(pipe->playing)) == 1) {
			playing = 1;
			break;
		}
	}
	return playing;
}
void play_file(const gchar *filename) {
	gchar path[256] = "file:///";
	g_print("playing %s\n", filename);
	int playing = is_playing();
	if(!playing) {
		PRINT("nothing is playing, setting to ready\n");
		gst_element_set_state (pipeline, GST_STATE_READY);
	}
	strncat(path, filename, 255);
	PipeData *pipe = make_pipe(path);
	gst_element_set_state (pipeline, GST_STATE_PLAYING);
}

static gboolean
gio_in (GIOChannel *gio, GIOCondition condition, gpointer data)
{
	GIOStatus ret;
	GError *err = NULL;
	gchar msg[8] = {0};
	gsize len;

	if (condition & G_IO_HUP)
		g_error ("Read end of pipe died!\n");

	ret = g_io_channel_read_chars (gio, msg, 1, &len, &err);
	if (ret == G_IO_STATUS_ERROR)
		g_error ("Error reading: %s\n", err->message);

	if(msg[0] - '1' >= 0 && msg[0] - '1' <= 5) {
		/* gst_element_set_state (pipeline, GST_STATE_READY); */
		PRINT("playing %c\n", msg[0]);
		play_file(BOARD[msg[0]-'1']);
	}
	else if(msg[0] == 'r') {
	}
	else if(msg[0] == 'q') {
		g_main_loop_quit (loop);
	}
	else if(msg[0] != '\n') {
		printf ("> %s\n", msg);
	}

	return TRUE;
}

void init_inputs() {
	GIOChannel *gio_read;
	int ret;
	gio_read = g_io_channel_unix_new(fileno(stdin));
	if(!gio_read) {
		g_error("failed creating channel: %s\n", strerror(errno));
	}
	ret = g_io_add_watch(gio_read, G_IO_IN | G_IO_HUP, gio_in, NULL);
	if(!ret) {
		g_error("failed adding watch: %s\n", strerror(errno));
	}
	set_shadow();
}

gboolean accept(gchar *filename, struct stat *buf) {
	if((buf->st_mode & S_IFMT) != S_IFREG) return FALSE;
	if(strstr(filename, ".wav")) return TRUE;
	if(strstr(filename, ".mp3")) return TRUE;
	return FALSE;
}

static gboolean
gio_readline (GIOChannel *gio, GIOCondition condition, gpointer data) {
	GIOStatus ret;
	GError *err = NULL;
	gchar *msg = NULL;
	gsize len;
	struct stat buf;

	if (condition & G_IO_HUP)
		g_error ("Read end of pipe died!\n");

	ret = g_io_channel_read_line (gio, &msg, &len, NULL, &err);
	if (ret == G_IO_STATUS_ERROR)
		g_error ("Error reading: %s\n", err->message);
	else if(msg == NULL) {
		g_warning ("NULL msg on pipe\n");
	}
	else {
		gchar *ptr = msg;
		while(*ptr != '\0') {
			if(*ptr == '\n') {
				*ptr = 0;
			}
			ptr++;
		}
		if(stat(msg, &buf) == 0) {
			if(accept(msg, &buf)) {
				play_file(msg);
			} else {
				g_print("not accepted: %s\n", msg);
			}
		} else {
			g_print("failed to find file: %s\n", msg);
		}
	}
	if(msg) {
		g_free(msg);
	}
	return TRUE;
}

void init_io() {
	mkfifo(pipefile, S_IRUSR | S_IWUSR);
	GIOChannel *gio_read;
	int fd = open(pipefile, O_RDWR | O_NONBLOCK);
	if(fd < 0) {
		g_error("failed opening pipe: %s\n", strerror(errno));
	}
	int ret;
	gio_read = g_io_channel_unix_new(fd);
	if(!gio_read) {
		g_error("failed creating channel: %s\n", strerror(errno));
	}
	ret = g_io_add_watch(gio_read, G_IO_IN | G_IO_HUP, gio_readline, NULL);
	if(!ret) {
		g_error("failed adding watch: %s\n", strerror(errno));
	}
}


PipeData* make_pipe(const gchar *filename) {
	PipeData *data = NULL;

	data = (PipeData*)g_async_queue_try_pop(free_pipes);
	if(data) {
		PRINT("reusing pipe %d\n", data->id);
	} else {
		data = g_new(PipeData, 1);
		data->id = pipecount++;
		g_atomic_int_set(&(data->free), 0);
		g_atomic_int_set(&(data->playing), 0);
		PRINT("new pipe %d\n", data->id);
		data->adder = adder;
		pipes = g_list_append(pipes, data);

		data->conv = gst_element_factory_make ("audioconvert", NULL);
		data->resample = gst_element_factory_make ("audioresample", NULL);
		data->caps = gst_element_factory_make ("capsfilter", NULL);
		gst_util_set_object_arg (G_OBJECT (data->caps), "caps",
				"audio/x-raw, rate=44000, format=S32LE, channels=2");

		gst_bin_add_many(GST_BIN (pipeline), data->conv, data->resample, data->caps, NULL);
		gst_element_link_many(data->conv, data->resample, data->caps, NULL);
	}
	GstPad *sink_pad = gst_element_get_request_pad (adder, "sink_%u");
	GstPad *caps_pad = gst_element_get_static_pad(data->caps, "src");
	gst_pad_link(caps_pad, sink_pad);
	gst_object_unref (sink_pad);
	gst_object_unref (caps_pad);

	data->src = gst_element_factory_make ("uridecodebin", NULL);
	g_object_set (data->src, "uri", filename, NULL);
	g_signal_connect(data->src, "pad-added", G_CALLBACK(pad_added_handler), data);
	gst_bin_add(GST_BIN (pipeline), data->src);
	/* gst_element_set_state(data->src, GST_STATE_PLAYING); */
	g_atomic_int_set(&(data->free), 0);
	g_atomic_int_set(&(data->playing), 0);

	return data;
}

int
main (int argc, char **argv)
{
	create_pid("audio-mixer", "/tmp/audio-mixer.pid", FD_CLOEXEC);
	GOptionEntry options[] = {
		{"pipefile", 'f', 0, G_OPTION_ARG_STRING, &opt_pipefile,
			"fifo file to listen to", NULL},
		{NULL}
	};
	GOptionContext *ctx;
	GError *err = NULL;
	GstElement *filter1, *resample, *sink;
	GstElement *testsrc;
	gchar **effect_names, **e;

	ctx = g_option_context_new ("");
	g_option_context_add_main_entries (ctx, options, NULL);
	g_option_context_add_group (ctx, gst_init_get_option_group ());
	if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
		g_print ("Error initializing: %s\n", err->message);
		g_clear_error (&err);
		g_option_context_free (ctx);
		return 1;
	}
	if(opt_pipefile == NULL) {
		pipefile = "/tmp/audio-mixer.pipe";
	} else {
		pipefile = strdup(opt_pipefile);
	}
	g_print("pipe file: %s\n", pipefile);
	g_option_context_free (ctx);
	free_pipes = g_async_queue_new();

#ifdef DEBUG
	init_inputs();
#endif
	init_io();

	pipeline = gst_pipeline_new ("pipeline");
	adder = gst_element_factory_make ("adder", NULL);
	conv = gst_element_factory_make ("audioconvert", NULL);
	resample = gst_element_factory_make ("audioresample", NULL);

	filter1 = gst_element_factory_make ("capsfilter", NULL);
	gst_util_set_object_arg (G_OBJECT (filter1), "caps",
			"audio/x-raw, rate=44000, format=S32LE, channels=2");

	testsrc = gst_element_factory_make ("audiotestsrc", NULL);
	g_assert(testsrc);
	g_object_set (testsrc, "wave", 8, NULL);
	sink = gst_element_factory_make ("autoaudiosink", NULL);

	gst_bin_add_many (GST_BIN (pipeline), adder, conv, resample, filter1, sink, NULL);
	gst_element_link_many (adder, conv, resample, filter1, sink, NULL);

	gst_element_set_state (pipeline, GST_STATE_PAUSED);
	loop = g_main_loop_new (NULL, FALSE);
	gst_bus_add_watch (GST_ELEMENT_BUS (pipeline), bus_cb, loop);

	/* g_timeout_add_seconds (1, timeout_cb, loop); */

	g_main_loop_run (loop);

	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (pipeline);
	g_async_queue_unref(free_pipes);

	return 0;
}

static void drained_handler (GstElement *src, GstPad *new_pad, void *data) {
	g_print ("drained\n");
}

static void pad_added_handler (GstElement *src, GstPad *new_pad, void *data) {
	/* GstElement *sink = GST_ELEMENT(data); */
	PipeData *pipe = (PipeData*)data;
	GstPadLinkReturn ret;
	if(g_atomic_int_get(&(pipe->free)) == 1) { /* shouldn't link anything, this is being removed */
		return;
	}
	GstPad *sink_pad = gst_element_get_static_pad (pipe->conv, "sink");
	PRINT ("Received new pad '%s' from '%s': pipe %d\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src), pipe->id);
	if (gst_pad_is_linked (sink_pad)) {
		g_print ("We are already linked. Ignoring.\n");
		goto exit;
	}
	ret = gst_pad_link (new_pad, sink_pad);

	if (GST_PAD_LINK_FAILED (ret)) {
		g_print ("[1;31mLink failed %d pipe: %d[m\n", ret, pipe->id);
		g_print ("[1mpipe: %d free: %d, playing %d[m\n", pipe->id, pipe->free, pipe->playing);
		pipe->src_pad = NULL;
		g_atomic_int_set(&(pipe->playing), 0);
		/* remove_src(pipe); */
	} else {
		PRINT ("Link succeeded\n");
		pipe->src_pad = new_pad;
		gst_pad_add_probe (new_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM , event_probe_cb, data, NULL);
	}
exit:
	gst_object_unref (sink_pad);
}


int
create_pid(const char *progName, const char *pidFile, int flags)
{
	int fd;
	char buf[64];

	fd = open(pidFile, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		g_print("Could not open PID file %s\n", pidFile);
		exit(1);
	}

	/* Set the close-on-exec file descriptor flag */

	/* Instead of the following steps, we could (on Linux) have opened the
	   file with O_CLOEXEC flag. However, not all systems support open()
	   O_CLOEXEC (which was standardized only in SUSv4), so instead we use
	   fcntl() to set the close-on-exec flag after opening the file */

	flags = fcntl(fd, F_GETFD);                     /* Fetch flags */
	if (flags == -1) {
		g_print("Could not get flags for PID file %s\n", pidFile);
		exit(1);
	}

	flags |= FD_CLOEXEC;                            /* Turn on FD_CLOEXEC */

	if (fcntl(fd, F_SETFD, flags) == -1) {
		g_print("Could not set flags for PID file %s\n", pidFile);
		exit(1);
	}           /* Update flags */

	struct flock fl;
	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	if (fcntl(fd, F_SETLK, &fl) == -1) {
		if (errno  == EAGAIN || errno == EACCES) {
			g_print("PID file '%s' is locked; probably "
					"'%s' is already running\n", pidFile, progName);
			exit(1);
		}
		else {
			g_print("Unable to lock PID file '%s'\n", pidFile);
			exit(1);
		}
	}

	if (ftruncate(fd, 0) == -1) {
		g_print("Could not truncate PID file '%s'", pidFile);
		exit(1);
	}

	snprintf(buf, 64, "%ld\n", (long) getpid());
	if (write(fd, buf, strlen(buf)) != strlen(buf)) {
		g_print("Writing to PID file '%s'", pidFile);
		exit(1);
	}

	return fd;
}
