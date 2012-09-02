/*
 * HairTunes - RAOP packet handler and slave-clocked replay engine
 * Copyright (c) James Laird 2011
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <openssl/aes.h>
#include <math.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <jni.h>
#include <android/log.h>

#ifdef FANCY_RESAMPLING
#include <samplerate.h>
#endif

#include <assert.h>
int debug = 0;

#include "alac.h"


#define LOG_TAG "Decoder"
#define LOG(level, msg) __android_log_write(level,LOG_TAG,msg)
#define LOG_INFO(msg) LOG(ANDROID_LOG_INFO,msg)
#define LOG_ERROR(msg) LOG(ANDROID_LOG_ERROR,msg)


// default buffer - about half a second
//Changed these values to make sound synchronized with airport express during multi-room broadcast.

#define BUFFER_FRAMES  512
#define START_FILL    282


#define MAX_PACKET      2048

typedef unsigned short seq_t;

// global options (constant after init)
unsigned char aeskey[16], aesiv[16];
AES_KEY aes;
char *rtphost = 0;
int dataport = 0, controlport = 0, timingport = 0;
int fmtp[32];
int sampling_rate;
int frame_size;


#define FRAME_BYTES (4*frame_size)
// maximal resampling shift - conservative
#define OUTFRAME_BYTES (4*(frame_size+3))

static signed short *outbuf = NULL;
static int g_abort = 0;
static alac_file *decoder_info = NULL;

#ifdef FANCY_RESAMPLING
int fancy_resampling = 1;
SRC_STATE *src;
#endif

static int  init_rtp(JNIEnv* env);
static void init_buffer(void);
static int  init_output(void);
static void rtp_request_resend(seq_t first, seq_t last);
static void ab_resync(void);
static short *buffer_get_frame(void);

// interthread variables
  // stdin->decoder
static volatile int mute = 0;
static volatile double volume = 1.0;
static volatile long fix_volume = 0x10000;

typedef struct audio_buffer_entry {   // decoded audio packets
    int ready;
    signed short *data;
} abuf_t;
static volatile abuf_t audio_buffer[BUFFER_FRAMES];
#define BUFIDX(seqno) ((seq_t)(seqno) % BUFFER_FRAMES)

// mutex-protected variables
volatile seq_t ab_read, ab_write;
int ab_buffering = 1, ab_synced = 0;
pthread_mutex_t ab_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ab_buffer_ready = PTHREAD_COND_INITIALIZER;
pthread_t rtp_thread;


typedef struct {
    double hist[2];
    double a[2];
    double b[3];
} biquad_t;

double bf_playback_rate = 1.0;
static double bf_est_drift = 0.0;   // local clock is slower by
static biquad_t bf_drift_lpf;
static double bf_est_err = 0.0, bf_last_err;
static biquad_t bf_err_lpf, bf_err_deriv_lpf;
static double desired_fill;
static int fill_count;


static int rtp_sockets[2];  // data, control
#ifdef AF_INET6
    struct sockaddr_in6 rtp_client;
#else
    struct sockaddr_in rtp_client;
#endif

static jclass exClass = NULL;

void die(JNIEnv* env, char *why) {

	LOG_ERROR(why);
	if(exClass) {
		(*env)->ThrowNew(env, exClass, why);
	}
}

void init_globby(void) {

	int i;

	g_abort = 0;

	mute = 0;
	volume = 1.0;
	fix_volume = 0x10000;

	bf_playback_rate = 1.0;
	bf_est_drift = 0.0;   // local clock is slower by
	bf_est_err = 0.0;

	ab_buffering = 1;
	ab_synced = 0;

	for (i = 0; i < BUFFER_FRAMES; i++) {
		audio_buffer[i].data = NULL;
	}

	pthread_mutex_init(&ab_mutex, NULL);
	pthread_cond_init(&ab_buffer_ready, NULL);
}


int init_decoder(JNIEnv* env) {
    alac_file *alac;

    decoder_info = NULL;

    frame_size = fmtp[1]; // stereo samples
    sampling_rate = fmtp[11];

    outbuf = malloc(OUTFRAME_BYTES);

    int sample_size = fmtp[3];
    if (sample_size != 16) {
        die(env, "only 16-bit samples supported!");
        return 0;
    }

    alac = create_alac(sample_size, 2);
    if (!alac) {
    	die(env, "cannot create alac");
    	return 0;
    }
    decoder_info = alac;

    alac->setinfo_max_samples_per_frame = frame_size;
    alac->setinfo_7a =      fmtp[2];
    alac->setinfo_sample_size = sample_size;
    alac->setinfo_rice_historymult = fmtp[4];
    alac->setinfo_rice_initialhistory = fmtp[5];
    alac->setinfo_rice_kmodifier = fmtp[6];
    alac->setinfo_7f =      fmtp[7];
    alac->setinfo_80 =      fmtp[8];
    alac->setinfo_82 =      fmtp[9];
    alac->setinfo_86 =      fmtp[10];
    alac->setinfo_8a_rate = fmtp[11];
    allocate_buffers(alac);
    return 1;
}


void flush(void) {
	LOG_INFO("flush");
	pthread_mutex_lock(&ab_mutex);
	ab_resync();
	pthread_mutex_unlock(&ab_mutex);
}


void Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeFlush( JNIEnv* env,
		jobject thiz) {
	flush();
}

void Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeSetVolume( JNIEnv* env,
		jobject thiz,
		jdouble jvolume) {
	LOG_INFO("set volume");
	// -144.0 is sent for mute. All other volume is in [-30.0..0.0] range
	if(jvolume < -30.0) {
		mute = 1;
	} else {
		mute = 0;
		volume = pow(10.0, 0.05*jvolume);
		fix_volume = 65536.0 * volume;
	}
}


void Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeSetMute(JNIEnv* env,
		jobject thiz,
		jboolean jmute) {
	LOG_INFO("mute");
    mute = jmute;
}


void Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeAbort( JNIEnv* env, jobject thiz) {
	LOG_INFO("abort");
	g_abort = 1;
	pthread_cond_signal(&ab_buffer_ready);
}


void Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeClose( JNIEnv* env,
		jobject thiz) {

	int i;

	LOG_INFO("close");

	//flush();

	if(decoder_info) {
		desallocate_buffers(decoder_info);
		free(decoder_info);
	}

	free(outbuf);

	for (i = 0; i < BUFFER_FRAMES; i++) {
	    free(audio_buffer[i].data);
	}
}

static jclass IOException_class = NULL;

jint Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeInit(JNIEnv* env,
		jobject thiz,
		jbyteArray aesivArray,
		jbyteArray aeskeyArray,
		jstring fmtpString,
		jint controlPort,
		jint timingPort,
		jint dataPort) {

    int i;
    char *arg;
    char *fmtpstr = 0;

    LOG_INFO("decodeInit: enter");

    exClass = (*env)->FindClass(env, "java/lang/Exception");
    if (!exClass) {
    	LOG_ERROR("cannot find class java.lang.Exception");
    	return 0;
    }

    init_globby();

    (*env)->GetByteArrayRegion(env, aesivArray, 0, 16, (jbyte *)aesiv);
    (*env)->GetByteArrayRegion(env, aeskeyArray, 0, 16, (jbyte *)aeskey);
    controlport = controlPort;
    timingport = timingPort;
    dataport = dataPort;

    AES_set_decrypt_key(aeskey, 128, &aes);

    memset(fmtp, 0, sizeof(fmtp));

    fmtpstr = (char *)(*env)->GetStringUTFChars(env, fmtpString, NULL);

    i = 0;
    while ( (arg = strsep(&fmtpstr, " \t")) ) {
        fmtp[i++] = atoi(arg);
    }

    (*env)->ReleaseStringUTFChars(env, fmtpString, fmtpstr);

    if(!init_decoder(env)) return 0;
    init_buffer();
    dataport = init_rtp(env);      // open a UDP listen port and start a listener; decode into ring buffer
    //init_output();              // resample and output from ring buffer

    LOG_INFO("decodeInit: exit");

    return dataport;
}


void init_buffer(void) {
    int i;
    for (i=0; i<BUFFER_FRAMES; i++)
        audio_buffer[i].data = malloc(OUTFRAME_BYTES);
    ab_resync();
}

void ab_resync(void) {
    int i;
    for (i=0; i<BUFFER_FRAMES; i++)
        audio_buffer[i].ready = 0;
    ab_synced = 0;
}

// the sequence numbers will wrap pretty often.
// this returns true if the second arg is after the first
static inline int seq_order(seq_t a, seq_t b) {
    signed short d = b - a;
    return d > 0;
}

void alac_decode(short *dest, char *buf, int len) {
    unsigned char packet[MAX_PACKET];
    assert(len<=MAX_PACKET);

    unsigned char iv[16];
    int i;
    memcpy(iv, aesiv, sizeof(iv));
    for (i=0; i+16<=len; i += 16)
        AES_cbc_encrypt((unsigned char*)buf+i, packet+i, 0x10, &aes, iv, AES_DECRYPT);
    if (len & 0xf)
        memcpy(packet+i, buf+i, len & 0xf);

    int outsize;

    decode_frame(decoder_info, packet, dest, &outsize);

    assert(outsize == FRAME_BYTES);
}

void buffer_put_packet(seq_t seqno, char *data, int len) {
    volatile abuf_t *abuf = 0;
    short read;
    short buf_fill;

    pthread_mutex_lock(&ab_mutex);
    if (!ab_synced) {
        ab_write = seqno;
        ab_read = seqno-1;
        ab_synced = 1;
    }
    if (seqno == ab_write+1) {                  // expected packet
        abuf = audio_buffer + BUFIDX(seqno);
        ab_write = seqno;
    } else if (seq_order(ab_write, seqno)) {    // newer than expected
        rtp_request_resend(ab_write, seqno-1);
        abuf = audio_buffer + BUFIDX(seqno);
        ab_write = seqno;
    } else if (seq_order(ab_read, seqno)) {     // late but not yet played
        abuf = audio_buffer + BUFIDX(seqno);
    } else {    // too late.
    	__android_log_print(ANDROID_LOG_WARN, LOG_TAG, "late packet %04X (%04X:%04X)", seqno, ab_read, ab_write);
    }
    buf_fill = ab_write - ab_read;
    pthread_mutex_unlock(&ab_mutex);

    if (abuf) {
        alac_decode(abuf->data, data, len);
        abuf->ready = 1;
    }

    if (ab_buffering && buf_fill >= START_FILL)
        pthread_cond_signal(&ab_buffer_ready);
    if (!ab_buffering) {
        // check if the t+10th packet has arrived... last-chance resend
        read = ab_read + 10;
        abuf = audio_buffer + BUFIDX(read);
        if (!abuf->ready)
            rtp_request_resend(read, read);
    }
}


void *rtp_thread_func(void *arg) {
    socklen_t si_len = sizeof(rtp_client);
    char packet[MAX_PACKET];
    char *pktp;
    seq_t seqno;
    ssize_t plen;
    int sock = rtp_sockets[0], csock = rtp_sockets[1];
    int readsock;
    char type;
    int rc;
    struct timeval tv;

    tv.tv_sec=0;
    tv.tv_usec=100000;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    FD_SET(csock, &fds);

    LOG_INFO("started RTP thread");

    while ((rc = select(csock>sock ? csock+1 : sock+1, &fds, 0, 0, &tv)) !=-1 && !g_abort) {

    	tv.tv_sec=0;
    	tv.tv_usec=100000;

    	if(rc == 0) {
    		FD_SET(sock, &fds);
    		FD_SET(csock, &fds);
    		continue;
    	}

        if (FD_ISSET(sock, &fds)) {
            readsock = sock;
        } else {
            readsock = csock;
        }
        FD_SET(sock, &fds);
        FD_SET(csock, &fds);

        plen = recvfrom(readsock, packet, sizeof(packet), 0, (struct sockaddr*)&rtp_client, &si_len);
        if (mute || plen < 0)
            continue;
        assert(plen<=MAX_PACKET);

        type = packet[1] & ~0x80;
        if (type == 0x60 || type == 0x56) {   // audio data / resend
            pktp = packet;
            if (type==0x56) {
                pktp += 4;
                plen -= 4;
            }
            seqno = ntohs(*(unsigned short *)(pktp+2));
            buffer_put_packet(seqno, pktp+12, plen-12);
        }
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "stopped RTP thread rc=%d abort=%d", rc, g_abort);

    return 0;
}

void rtp_request_resend(seq_t first, seq_t last) {
    if (seq_order(last, first))
        return;

    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "requesting resend on %d packets (port %d)", last-first+1, controlport);

    char req[8];    // *not* a standard RTCP NACK
    req[0] = 0x80;
    req[1] = 0x55|0x80;  // Apple 'resend'
    *(unsigned short *)(req+2) = htons(1);  // our seqnum
    *(unsigned short *)(req+4) = htons(first);  // missed seqnum
    *(unsigned short *)(req+6) = htons(last-first+1);  // count

#ifdef AF_INET6
    rtp_client.sin6_port = htons(controlport);
#else
    rtp_client.sin_port = htons(controlport);
#endif
    sendto(rtp_sockets[1], req, sizeof(req), 0, (struct sockaddr *)&rtp_client, sizeof(struct sockaddr_in));
}


int init_rtp(JNIEnv* env) {
    struct sockaddr_in si;
    int type = AF_INET;
	struct sockaddr* si_p = (struct sockaddr*)&si;
	socklen_t si_len = sizeof(si);
    unsigned short *sin_port = &si.sin_port;
    memset(&si, 0, sizeof(si));
#ifdef AF_INET6
    struct sockaddr_in6 si6;
    type = AF_INET6;
	si_p = (struct sockaddr*)&si6;
	si_len = sizeof(si6);
    sin_port = &si6.sin6_port;
    memset(&si6, 0, sizeof(si6));
#endif

    si.sin_family = AF_INET;
#ifdef SIN_LEN
	si.sin_len = sizeof(si);
#endif
    si.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef AF_INET6
    si6.sin6_family = AF_INET6;
    #ifdef SIN6_LEN
        si6.sin6_len = sizeof(si);
    #endif
    si6.sin6_addr = in6addr_any;
    si6.sin6_flowinfo = 0;
#endif

    int sock = -1, csock = -1;    // data and control (we treat the streams the same here)
    unsigned short port = 6000;


    while(1) {
        if(sock < 0)
            sock = socket(type, SOCK_DGRAM, IPPROTO_UDP);
#ifdef AF_INET6
	    if(sock==-1 && type == AF_INET6) {
	        // try fallback to IPv4
	        type = AF_INET;
	        si_p = (struct sockaddr*)&si;
	        si_len = sizeof(si);
	        sin_port = &si.sin_port;
	        continue;
	    }
#endif
        if (sock==-1) {
            die(env, "can't create data socket!");
            return 0;
        }

        if(csock < 0)
            csock = socket(type, SOCK_DGRAM, IPPROTO_UDP);
        if (csock==-1) {
            die(env, "can't create control socket!");
            return 0;
        }

        *sin_port = htons(port);
        int bind1 = bind(sock, si_p, si_len);
        *sin_port = htons(port + 1);
        int bind2 = bind(csock, si_p, si_len);

        if(bind1 != -1 && bind2 != -1) break;
        if(bind1 != -1) { close(sock); sock = -1; }
        if(bind2 != -1) { close(csock); csock = -1; }

        port += 3;
    }


    rtp_sockets[0] = sock;
    rtp_sockets[1] = csock;
    pthread_create(&rtp_thread, NULL, rtp_thread_func, (void *)rtp_sockets);

    return port;
}

static inline short dithered_vol(short sample) {
    static short rand_a, rand_b;
    long out;
    rand_b = rand_a;
    rand_a = rand() & 0xffff;

    out = (long)sample * fix_volume;
    if (fix_volume < 0x10000) {
        out += rand_a;
        out -= rand_b;
    }
    return out>>16;
}


static void biquad_init(biquad_t *bq, double a[], double b[]) {
    bq->hist[0] = bq->hist[1] = 0.0;
    memcpy(bq->a, a, 2*sizeof(double));
    memcpy(bq->b, b, 3*sizeof(double));
}

static void biquad_lpf(biquad_t *bq, double freq, double Q) {
    double w0 = 2*M_PI*freq/((float)sampling_rate/(float)frame_size);
    double alpha = sin(w0)/(2.0*Q);

    double a_0 = 1.0 + alpha;
    double b[3], a[2];
    b[0] = (1.0-cos(w0))/(2.0*a_0);
    b[1] = (1.0-cos(w0))/a_0;
    b[2] = b[0];
    a[0] = -2.0*cos(w0)/a_0;
    a[1] = (1-alpha)/a_0;

    biquad_init(bq, a, b);
}

static double biquad_filt(biquad_t *bq, double in) {
    double w = in - bq->a[0]*bq->hist[0] - bq->a[1]*bq->hist[1];
    double out __attribute__((unused)) = bq->b[1]*bq->hist[0] + bq->b[2]*bq->hist[1] + bq->b[0]*w;
    bq->hist[1] = bq->hist[0];
    bq->hist[0] = w;

    return w;
}


void bf_est_reset(short fill) {
    biquad_lpf(&bf_drift_lpf, 1.0/180.0, 0.3);
    biquad_lpf(&bf_err_lpf, 1.0/10.0, 0.25);
    biquad_lpf(&bf_err_deriv_lpf, 1.0/2.0, 0.2);
    fill_count = 0;
    bf_playback_rate = 1.0;
    bf_est_err = bf_last_err = 0;
    desired_fill = fill_count = 0;
}
void bf_est_update(short fill) {
    if (fill_count < 1000) {
        desired_fill += (double)fill/1000.0;
        fill_count++;
        return;
    }

#define CONTROL_A   (1e-4)
#define CONTROL_B   (1e-1)

    double buf_delta = fill - desired_fill;
    bf_est_err = biquad_filt(&bf_err_lpf, buf_delta);
    double err_deriv = biquad_filt(&bf_err_deriv_lpf, bf_est_err - bf_last_err);

    bf_est_drift = biquad_filt(&bf_drift_lpf, CONTROL_B*(bf_est_err*CONTROL_A + err_deriv) + bf_est_drift);

    if (debug)
        fprintf(stderr, "bf %d err %f drift %f desiring %f ed %f estd %f\r", fill, bf_est_err, bf_est_drift, desired_fill, err_deriv, err_deriv + CONTROL_A*bf_est_err);
    bf_playback_rate = 1.0 + CONTROL_A*bf_est_err + bf_est_drift;

    bf_last_err = bf_est_err;
}

// get the next frame, when available. return 0 if underrun/stream reset.
short *buffer_get_frame(void) {
    short buf_fill;
    seq_t read;

    pthread_mutex_lock(&ab_mutex);

    buf_fill = ab_write - ab_read;
    if (buf_fill < 1 || !ab_synced) {    // init or underrun. stop and wait
        if (ab_synced)
        	LOG_INFO("buffer_get_frame: underrun");

        ab_buffering = 1;
        pthread_cond_wait(&ab_buffer_ready, &ab_mutex);
        if(g_abort) {
        	LOG_INFO("buffer_get_frame: aborted");
        	pthread_mutex_unlock(&ab_mutex);
        	return 0;
        }

        ab_read++;
        buf_fill = ab_write - ab_read;
        pthread_mutex_unlock(&ab_mutex);

        bf_est_reset(buf_fill);
        return 0;
    }
    if (buf_fill >= BUFFER_FRAMES) {   // overrunning! uh-oh. restart at a sane distance
    	LOG_INFO("buffer_get_frame: overrun");
        ab_read = ab_write - START_FILL;
    }
    read = ab_read;
    ab_read++;
    pthread_mutex_unlock(&ab_mutex);

    buf_fill = ab_write - ab_read;
    bf_est_update(buf_fill);

    volatile abuf_t *curframe = audio_buffer + BUFIDX(read);
    if (!curframe->ready) {
    	LOG_ERROR("buffer_get_frame: missing frame");
        memset(curframe->data, 0, FRAME_BYTES);
    }
    curframe->ready = 0;
    return curframe->data;
}

int stuff_buffer(double playback_rate, short *inptr, short *outptr) {
    int i;
    int stuffsamp = frame_size;
    int stuff = 0;
    double p_stuff;

    p_stuff = 1.0 - pow(1.0 - fabs(playback_rate-1.0), frame_size);

    if ((float)rand()/((float)RAND_MAX) < p_stuff) {
        stuff = playback_rate > 1.0 ? -1 : 1;
        stuffsamp = rand() % (frame_size - 1);
    }

    for (i=0; i<stuffsamp; i++) {   // the whole frame, if no stuffing
        *outptr++ = dithered_vol(*inptr++);
        *outptr++ = dithered_vol(*inptr++);
    };
    if (stuff) {
        if (stuff==1) {
            if (debug)
                fprintf(stderr, "+++++++++\n");
            // interpolate one sample
            *outptr++ = dithered_vol(((long)inptr[-2] + (long)inptr[0]) >> 1);
            *outptr++ = dithered_vol(((long)inptr[-1] + (long)inptr[1]) >> 1);
        } else if (stuff==-1) {
            if (debug)
                fprintf(stderr, "---------\n");
            inptr++;
            inptr++;
        }
        for (i=stuffsamp; i<frame_size + stuff; i++) {
            *outptr++ = dithered_vol(*inptr++);
            *outptr++ = dithered_vol(*inptr++);
        }
    }

    return frame_size + stuff;
}


jint Java_com_bubblesoft_android_airbubble_AndroidDecoder_decodeGetAudioChunk( JNIEnv* env, jobject thiz, jbyteArray audioChunk) {
	int play_samples;

	signed short buf_fill __attribute__((unused));
	signed short *inbuf;

#ifdef FANCY_RESAMPLING
	float *frame, *outframe;
	SRC_DATA srcdat;
	if (fancy_resampling) {
		frame = malloc(frame_size*2*sizeof(float));
		outframe = malloc(2*frame_size*2*sizeof(float));

		srcdat.data_in = frame;
		srcdat.data_out = outframe;
		srcdat.input_frames = FRAME_BYTES;
		srcdat.output_frames = 2*FRAME_BYTES;
		srcdat.src_ratio = 1.0;
		srcdat.end_of_input = 0;
	}
#endif

	do {
		inbuf = buffer_get_frame();
		if(g_abort) return -1;
	} while (!inbuf);

#ifdef FANCY_RESAMPLING
if (fancy_resampling) {
	int i;
	for (i=0; i<2*FRAME_BYTES; i++) {
		frame[i] = (float)inbuf[i] / 32768.0;
		frame[i] *= volume;
	}
	srcdat.src_ratio = bf_playback_rate;
	src_process(src, &srcdat);
	assert(srcdat.input_frames_used == FRAME_BYTES);
	src_float_to_short_array(outframe, outbuf, FRAME_BYTES*2);
	play_samples = srcdat.output_frames_gen;
} else
#endif

	play_samples = stuff_buffer(bf_playback_rate, inbuf, outbuf);

	//ao_play(dev, (char *)outbuf, play_samples*4);
	(*env)->SetByteArrayRegion(env, audioChunk, 0, play_samples*4, (jbyte *)outbuf);


	return play_samples*4;
}



#define NUM_CHANNELS 2


/*
void* init_ao() {
    ao_initialize();
    int driver = ao_default_driver_id();

    ao_sample_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.bits = 16;
    fmt.rate = sampling_rate;
    fmt.channels = NUM_CHANNELS;
    fmt.byte_format = AO_FMT_NATIVE;

    ao_device *dev = ao_open_live(driver, &fmt, 0);
    return dev;
}


int init_output(void) {
	void* arg = 0;


#ifdef FANCY_RESAMPLING
    if (fancy_resampling)
        src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &err);
    else
        src = 0;
#endif

    return 0;
}
*/

