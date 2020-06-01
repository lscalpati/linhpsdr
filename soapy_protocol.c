/* Copyright (C)
* 2019 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wdsp.h>

#include "SoapySDR/Constants.h"
#include "SoapySDR/Device.h"
#include "SoapySDR/Formats.h"
#include "SoapySDR/Version.h"

//#define TIMING
#ifdef TIMING
#include <sys/time.h>
#endif

#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "bpsk.h"
#include "mode.h"
#include "filter.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "protocol1.h"
#include "soapy_protocol.h"
#include "audio.h"
#include "signal.h"
#include "vfo.h"
#include "ext.h"
#include "error_handler.h"

static double bandwidth=1000000.0;

static SoapySDRDevice *soapy_device;
static SoapySDRStream *rx_stream;
static SoapySDRStream *tx_stream;
static int soapy_rx_sample_rate;
static int soapy_tx_sample_rate;
static int max_samples;

static int samples=0;

static GThread *receive_thread_id;
static gpointer receive_thread(gpointer data);

static int actual_rate;

#ifdef TIMING
static int rate_samples;
#endif

static gboolean running;

static int mic_sample_divisor=1;


static int max_tx_samples;
static float *output_buffer;
static int output_buffer_index;

SoapySDRDevice *get_soapy_device() {
  return soapy_device;
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

void soapy_protocol_change_sample_rate(RECEIVER *rx,int rate) {
}

void soapy_protocol_create_receiver(RECEIVER *rx) {
  int rc;

  soapy_rx_sample_rate=rx->sample_rate;

fprintf(stderr,"soapy_protocol_create: setting bandwidth=%f\n",bandwidth);
  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_RX,rx->adc,bandwidth);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setBandwidth(%f) failed: %s\n",(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

fprintf(stderr,"soapy_protocol_create: setting samplerate=%f\n",(double)soapy_rx_sample_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc,(double)soapy_rx_sample_rate);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

  size_t channel=rx->adc;
fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream: channel=%ld\n",channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&rx_stream,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  rx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rx_stream==NULL) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif


  max_samples=SoapySDRDevice_getStreamMTU(soapy_device,rx_stream);
  if(max_samples>(2*rx->fft_size)) {
    max_samples=2*rx->fft_size;
  }
  rx->buffer=g_new(double,max_samples*2);

}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  int rc;

fprintf(stderr,"soapy_protocol_start_receiver: activate_stream\n");
  rc=SoapySDRDevice_activateStream(soapy_device, rx_stream, 0, 0LL, 0);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_start_receiver: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }

fprintf(stderr,"soapy_protocol_start_receiver: create receive_thread\n");
  receive_thread_id = g_thread_new( "rx_thread", receive_thread, rx);
  if( ! receive_thread_id )
  {
    fprintf(stderr,"g_thread_new failed for receive_thread\n");
    exit( -1 );
  }
  fprintf(stderr, "receive_thread: id=%p\n",receive_thread_id);
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  int rc;

fprintf(stderr,"soapy_protocol_create_transmitter: dac=%d\n",tx->dac);

  soapy_tx_sample_rate=tx->iq_output_rate;

fprintf(stderr,"soapy_protocol_create_transmitter: setting bandwidth=%f\n",bandwidth);

  rc=SoapySDRDevice_setBandwidth(soapy_device,SOAPY_SDR_TX,tx->dac,bandwidth);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setBandwidth(%f) failed: %s\n",(double)soapy_rx_sample_rate,SoapySDR_errToStr(rc));
  }

fprintf(stderr,"soapy_protocol_create_transmitter: setting samplerate=%f\n",(double)soapy_tx_sample_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac,(double)soapy_tx_sample_rate);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_configure_transmitter: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)soapy_tx_sample_rate,SoapySDR_errToStr(rc));
  }

  size_t channel=tx->dac;
fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream: channel=%ld\n",channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&tx_stream,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  tx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(tx_stream==NULL) {
    fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif

  max_tx_samples=SoapySDRDevice_getStreamMTU(soapy_device,tx_stream);
  if(max_tx_samples>(2*tx->fft_size)) {
    max_tx_samples=2*tx->fft_size;
  }
fprintf(stderr,"soapy_protocol_create_transmitter: max_tx_samples=%d\n",max_tx_samples);
  output_buffer=(float *)malloc(max_tx_samples*sizeof(float)*2);

}

void soapy_protocol_start_transmitter(TRANSMITTER *tx) {
  int rc;

fprintf(stderr,"soapy_protocol_start_transmitter: activateStream\n");
  rc=SoapySDRDevice_activateStream(soapy_device, tx_stream, 0, 0LL, 0);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_start_transmitter: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_stop_transmitter(TRANSMITTER *tx) {
  int rc;

fprintf(stderr,"soapy_protocol_stop_transmitter: deactivateStream\n");
  rc=SoapySDRDevice_deactivateStream(soapy_device, tx_stream, 0, 0LL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_stop_transmitter: SoapySDRDevice_deactivateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_init(RADIO *r,int rx) {
  SoapySDRKwargs args={};
  int rc;
  int i;

fprintf(stderr,"soapy_protocol_init\n");

  // initialize the radio
fprintf(stderr,"soapy_protocol_init: SoapySDRDevice_make\n");
  SoapySDRKwargs_set(&args, "driver", radio->discovered->name);
  if(strcmp(radio->discovered->name,"rtlsdr")==0) {
    char id[16];
    sprintf(id,"%d",radio->discovered->info.soapy.rtlsdr_count);
    SoapySDRKwargs_set(&args, "rtl", id);
  }
  soapy_device=SoapySDRDevice_make(&args);
  if(soapy_device==NULL) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_make failed: %s\n",SoapySDRDevice_lastError());
    _exit(-1);
  }
  SoapySDRKwargs_clear(&args);

}

static void *receive_thread(void *arg) {
  double isample;
  double qsample;
  int elements;
  int flags=0;
  long long timeNs=0;
  long timeoutUs=100000L;
  int i;
#ifdef TIMING
  struct timeval tv;
  long start_time, end_time;
  rate_samples=0;
  gettimeofday(&tv, NULL); start_time=tv.tv_usec + 1000000 * tv.tv_sec;
#endif
  RECEIVER *rx=(RECEIVER *)arg;
  float *buffer=g_new(float,max_samples*2);
  void *buffs[]={buffer};
  running=TRUE;
fprintf(stderr,"soapy_protocol: receive_thread\n");
  while(running) {
    elements=SoapySDRDevice_readStream(soapy_device,rx_stream,buffs,max_samples,&flags,&timeNs,timeoutUs);
    if(elements<0) {
      fprintf(stderr,"elements=%d max_samples=%d\n",elements,max_samples);
    }
    for(i=0;i<elements;i++) {
      rx->buffer[i*2]=(double)buffer[i*2];
      rx->buffer[(i*2)+1]=(double)buffer[(i*2)+1];
    }
/*
    if(rx->resampler!=NULL) {
      int out_elements=xresample(rx->resampler);
g_print("resampler: elements in=%d out=%d\n",elements,out_elements);
      for(i=0;i<out_elements;i++) {
        if(radio->iqswap) {
          qsample=rx->resampled_buffer[i*2];
          isample=rx->resampled_buffer[(i*2)+1];
        } else {
          isample=rx->resampled_buffer[i*2];
          qsample=rx->resampled_buffer[(i*2)+1];
        }
        add_iq_samples(rx,isample,qsample);
      }
*/
    if(rx->sample_rate!=radio->sample_rate) {
      for(int i=0;i<elements;i+=rx->resample_step) {
/*
        isample=0.0;
        qsample=0.0;
        for(int j=0;j<rx->resample_step;j++) {
          isample+=rx->buffer[(i+j)*2];
          qsample+=rx->buffer[((i+j)*2)+1];
        }
        if(radio->iqswap) {
          add_iq_samples(rx,qsample/(double)rx->resample_step,isample/(double)rx->resample_step);
        } else {
          add_iq_samples(rx,isample/(double)rx->resample_step,qsample/(double)rx->resample_step);
        }
*/
        isample=rx->buffer[i*2];
        qsample=rx->buffer[(i*2)+1];
        if(radio->iqswap) {
          add_iq_samples(rx,qsample,isample);
        } else {
          add_iq_samples(rx,isample,qsample);
        }
      }
    } else {
      for(i=0;i<elements;i++) {
        isample=rx->buffer[i*2];
        qsample=rx->buffer[(i*2)+1];
        if(radio->iqswap) {
          add_iq_samples(rx,qsample,isample);
        } else {
          add_iq_samples(rx,isample,qsample);
        }
#ifdef TIMING
        rate_samples++;
        if(rate_samples>=rx->sample_rate) {
          gettimeofday(&tv, NULL); end_time=tv.tv_usec + 1000000 * tv.tv_sec;
          fprintf(stderr,"%d samples in %ld usec\n",rx->sample_rate,end_time-start_time);
          rate_samples=0;
          start_time=end_time;
        }
#endif
      }
    }
  }

fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_deactivateStream\n");
  SoapySDRDevice_deactivateStream(soapy_device,rx_stream,0,0LL);
fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_closeStream\n");
  SoapySDRDevice_closeStream(soapy_device,rx_stream);
fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_unmake\n");
  SoapySDRDevice_unmake(soapy_device);
  _exit(0);
}

void soapy_protocol_process_local_mic(RADIO *r) {
  int b;
  int i;
  short sample;

// always 48000 samples per second
  b=0;
  for(i=0;i<r->local_microphone_buffer_size;i++) {
    add_mic_sample(r->transmitter,r->local_microphone_buffer[i]);
  }
}

void soapy_protocol_iq_samples(float isample,float qsample) {
  const void *tx_buffs[]={output_buffer};
  int flags=0;
  long long timeNs=0;
  long timeoutUs=100000L;
  if(isTransmitting(radio)) {
    output_buffer[output_buffer_index*2]=isample;
    output_buffer[(output_buffer_index*2)+1]=qsample;
    output_buffer_index++;
    if(output_buffer_index>=max_tx_samples) {
      int elements=SoapySDRDevice_writeStream(soapy_device,tx_stream,tx_buffs,max_tx_samples,&flags,timeNs,timeoutUs);
      if(elements!=max_tx_samples) {
        g_print("soapy_protocol_iq_samples: writeStream returned %d for %d elements\n",elements,max_tx_samples);
      }

      output_buffer_index=0;
    }
  }
}



void soapy_protocol_stop() {
fprintf(stderr,"soapy_protocol_stop\n");
  running=FALSE;
}

void soapy_protocol_set_rx_frequency(RECEIVER *rx) {
  int rc;

  if(soapy_device!=NULL) {
    double f=(double)(rx->frequency_a-rx->lo_a+rx->error_a);
    if(!rx->ctun) {
      if(rx->rit_enabled) {
        f+=(double)rx->rit;
      }
    }
    rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_RX,rx->adc,f,NULL);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setFrequency(RX) failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int rc;
  double f;

  if(soapy_device!=NULL) {
    RECEIVER* rx=tx->rx;
    if(rx!=NULL) {
      if(rx->split) {
        f=rx->frequency_b-rx->lo_b+rx->error_b;
      } else {
        if(rx->ctun) {
          f=rx->ctun_frequency-rx->lo_a+rx->error_a;
        } else {
          f=rx->frequency_a-rx->lo_a+rx->error_a;
        }
      }
      if(tx->xit_enabled) {
        f+=(double)tx->xit;
      }
g_print("soapy_protocol_set_frequency: %f\n",f);
      rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_TX,tx->dac,f,NULL);
      if(rc!=0) {
        fprintf(stderr,"soapy_protocol: SoapySDRDevice_setFrequency(TX) failed: %s\n",SoapySDR_errToStr(rc));
      }
    }
  }
}

void soapy_protocol_set_rx_antenna(RECEIVER *rx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    fprintf(stderr,"soapy_protocol: set_rx_antenna: %s\n",radio->discovered->info.soapy.rx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_RX,rx->adc,radio->discovered->info.soapy.rx_antenna[ant]);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setAntenna RX failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_antenna(TRANSMITTER *tx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    fprintf(stderr,"soapy_protocol: set_tx_antenna: %s\n",radio->discovered->info.soapy.tx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_TX,tx->dac,radio->discovered->info.soapy.tx_antenna[ant]);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setAntenna TX failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_gain(ADC *adc) {
  int rc;
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_RX,adc->id,adc->gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGain failed: %s\n",SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain(DAC *dac) {
  int rc;
g_print("%s: dac=%d gain=%f\n",__FUNCTION__,dac->id,dac->gain);
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,dac->id,dac->gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGain failed: %s\n",SoapySDR_errToStr(rc));
  }
}

int soapy_protocol_get_gain(ADC *adc) {
  double gain;
  gain=SoapySDRDevice_getGain(soapy_device,SOAPY_SDR_RX,adc->id);
  return (int)gain;
}

gboolean soapy_protocol_get_automatic_gain(ADC *adc) {
  gboolean mode=SoapySDRDevice_getGainMode(soapy_device, SOAPY_SDR_RX, adc->id);
  return mode;
}

void soapy_protocol_set_automatic_gain(RECEIVER *rx,gboolean mode) {
  int rc;
  rc=SoapySDRDevice_setGainMode(soapy_device, SOAPY_SDR_RX, rx->adc,mode);
  if(rc!=0) {

    fprintf(stderr,"soapy_protocol: SoapySDRDevice_getGainMode failed: %s\n", SoapySDR_errToStr(rc));
  }
}

char *soapy_protocol_read_sensor(char *name) {
  return SoapySDRDevice_readSensor(soapy_device, name);
}
