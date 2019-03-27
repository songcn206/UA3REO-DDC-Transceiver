#include "audio_processor.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "arm_math.h"
#include "fpga.h"
#include "trx_manager.h"
#include "wm8731.h"
#include "functions.h"
#include "audio_filters.h"
#include "agc.h"
#include "settings.h"
#include "profiler.h"
#include <complex.h>

uint32_t AUDIOPROC_samples = 0;
uint32_t AUDIOPROC_TXA_samples = 0;
uint32_t AUDIOPROC_TXB_samples = 0;
int32_t Processor_AudioBuffer_A[FPGA_AUDIO_BUFFER_SIZE] = { 0 };
int32_t Processor_AudioBuffer_B[FPGA_AUDIO_BUFFER_SIZE] = { 0 };
uint8_t Processor_AudioBuffer_ReadyBuffer = 0;
bool Processor_NeedRXBuffer = false;
bool Processor_NeedTXBuffer = false;
float32_t FPGA_Audio_Buffer_Q_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = { 0 };
float32_t FPGA_Audio_Buffer_I_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = { 0 };
const uint16_t numBlocks = FPGA_AUDIO_BUFFER_HALF_SIZE / APROCESSOR_BLOCK_SIZE;
uint16_t block = 0;

float32_t Processor_AVG_amplitude = 0.0f; //средняя амплитуда семплов при прёме
float32_t Processor_TX_MAX_amplitude = 0.0f; //средняя амплитуда семплов при передаче
float32_t Processor_RX_Audio_Samples_MAX_value = 0.0f; //максимальное значение семплов
float32_t Processor_RX_Audio_Samples_MIN_value = 0.0f; //минимальное значение семплов
float32_t ampl_val_i = 0.0f;
float32_t ampl_val_q = 0.0f;
float32_t selected_rfpower_amplitude = 0.0f;
float32_t ALC_need_gain = 1.0f;
float32_t ALC_need_gain_new = 1.0f;
float32_t fm_sql_avg = 0.0f;
static float32_t i_prev, q_prev;// used in FM detection and low/high pass processing
static uint8_t fm_sql_count = 0;// used for squelch processing and debouncing tone detection, respectively

void initAudioProcessor(void)
{
	InitFilters();
	SetupAgcWdsp(); //AGC
}

void processTxAudio(void)
{
	if (!Processor_NeedTXBuffer) return;
	AUDIOPROC_samples++;
	selected_rfpower_amplitude = TRX.RF_Power / 100.0f * MAX_TX_AMPLITUDE;
	
	uint16_t dma_index=CODEC_AUDIO_BUFFER_SIZE-__HAL_DMA_GET_COUNTER(hi2s3.hdmarx)/2;
	if((dma_index%2)==1) dma_index++;
	readHalfFromCircleBufferU32((uint32_t *)&CODEC_Audio_Buffer_TX[0], (uint32_t *)&Processor_AudioBuffer_A[0], dma_index, CODEC_AUDIO_BUFFER_SIZE);

	for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		if (TRX_tune)
		{
			FPGA_Audio_Buffer_Q_tmp[i] = TUNE_AMPLITUDE;
			FPGA_Audio_Buffer_I_tmp[i] = TUNE_AMPLITUDE;
		}
		else
		{
			FPGA_Audio_Buffer_I_tmp[i] = (int16_t)(Processor_AudioBuffer_A[i * 2]);
			FPGA_Audio_Buffer_Q_tmp[i] = (int16_t)(Processor_AudioBuffer_A[i * 2 + 1]);
		}
	}

	if (TRX_getMode() != TRX_MODE_IQ && !TRX_tune)
	{
		//IIR HPF
		for (block = 0; block < numBlocks; block++) arm_iir_lattice_f32(&IIR_HPF, (float32_t *)&FPGA_Audio_Buffer_I_tmp[block*APROCESSOR_BLOCK_SIZE], (float32_t *)&FPGA_Audio_Buffer_I_tmp[block*APROCESSOR_BLOCK_SIZE], APROCESSOR_BLOCK_SIZE);
		//IIR LPF
		for (block = 0; block < numBlocks; block++) arm_iir_lattice_f32(&IIR_LPF_I, (float32_t *)&FPGA_Audio_Buffer_I_tmp[block*APROCESSOR_BLOCK_SIZE], (float32_t *)&FPGA_Audio_Buffer_I_tmp[block*APROCESSOR_BLOCK_SIZE], APROCESSOR_BLOCK_SIZE);
		memcpy(&FPGA_Audio_Buffer_Q_tmp[0], &FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE * 4); //double left and right channel

		if(TRX_getMode() != TRX_MODE_LOOPBACK && TRX_getMode() != TRX_MODE_IQ)
		{	
			//RF PowerControl (Audio Level Control) Compressor
			Processor_TX_MAX_amplitude=0;
			if (TRX_tune) ALC_need_gain=1;
			for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
			{
				arm_abs_f32(&FPGA_Audio_Buffer_I_tmp[i], &ampl_val_i, 1);
				arm_abs_f32(&FPGA_Audio_Buffer_Q_tmp[i], &ampl_val_q, 1);
				if (ampl_val_i > Processor_TX_MAX_amplitude) Processor_TX_MAX_amplitude=ampl_val_i;
				if (ampl_val_q > Processor_TX_MAX_amplitude) Processor_TX_MAX_amplitude=ampl_val_q;
			}
			if(Processor_TX_MAX_amplitude==0.0f) Processor_TX_MAX_amplitude=0.001f;
			ALC_need_gain_new = selected_rfpower_amplitude / Processor_TX_MAX_amplitude;
			if(ALC_need_gain_new>ALC_need_gain)
				ALC_need_gain += TX_AGC_STEPSIZE;
			else if(ALC_need_gain_new<ALC_need_gain)
				ALC_need_gain -= TX_AGC_STEPSIZE;
			
			if(ALC_need_gain>TX_AGC_MAXGAIN) ALC_need_gain=TX_AGC_MAXGAIN;
			if(Processor_TX_MAX_amplitude<TX_AGC_NOISEGATE) ALC_need_gain=0.0f;
			
			arm_scale_f32(FPGA_Audio_Buffer_I_tmp, ALC_need_gain, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_scale_f32(FPGA_Audio_Buffer_Q_tmp, ALC_need_gain, FPGA_Audio_Buffer_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			//
			switch (TRX_getMode())
			{
				case TRX_MODE_CW_L:
				case TRX_MODE_CW_U:
					if(!TRX_key_serial && !TRX_ptt_hard) selected_rfpower_amplitude=0;
					for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
					{
						FPGA_Audio_Buffer_Q_tmp[i] = selected_rfpower_amplitude;
						FPGA_Audio_Buffer_I_tmp[i] = selected_rfpower_amplitude;
					}
					break;
				case TRX_MODE_USB:
				case TRX_MODE_DIGI_U:
					for (block = 0; block < numBlocks; block++)
					{
						// + 45 deg to Q data
						arm_fir_f32(&FIR_TX_Hilbert_Q, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE);
						// - 45 deg to I data
						arm_fir_f32(&FIR_TX_Hilbert_I, (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE); 
					}
					break;
				case TRX_MODE_LSB:
				case TRX_MODE_DIGI_L:
					//hilbert fir
					for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
						FPGA_Audio_Buffer_Q_tmp[i] = FPGA_Audio_Buffer_I_tmp[i];
					for (block = 0; block < numBlocks; block++)
					{
						// + 45 deg to I data
						arm_fir_f32(&FIR_TX_Hilbert_I, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE);
						// - 45 deg to Q data
						arm_fir_f32(&FIR_TX_Hilbert_Q, (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE); 
					}					
					break;
				case TRX_MODE_AM:
					for (block = 0; block < numBlocks; block++)
					{
						// + 45 deg to I data
						arm_fir_f32(&FIR_TX_Hilbert_I, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE);
						// - 45 deg to Q data
						arm_fir_f32(&FIR_TX_Hilbert_Q, (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE); 
					}
					for (size_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
					{
						float32_t i_am = ((FPGA_Audio_Buffer_I_tmp[i] - FPGA_Audio_Buffer_Q_tmp[i]) + (selected_rfpower_amplitude))/2.0f;
						float32_t q_am = ((FPGA_Audio_Buffer_Q_tmp[i] - FPGA_Audio_Buffer_I_tmp[i]) - (selected_rfpower_amplitude))/2.0f;
						FPGA_Audio_Buffer_I_tmp[i] = i_am;
						FPGA_Audio_Buffer_Q_tmp[i] = q_am;
					}
					break;
				case TRX_MODE_NFM:
				case TRX_MODE_WFM:
					ModulateFM();
					break;
				default:
				break;
			}
			
			//signal limiter
			for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
			{
				if(FPGA_Audio_Buffer_I_tmp[i]>selected_rfpower_amplitude) FPGA_Audio_Buffer_I_tmp[i]=selected_rfpower_amplitude;
				if(FPGA_Audio_Buffer_Q_tmp[i]>selected_rfpower_amplitude) FPGA_Audio_Buffer_Q_tmp[i]=selected_rfpower_amplitude;
				if(FPGA_Audio_Buffer_I_tmp[i]<-selected_rfpower_amplitude) FPGA_Audio_Buffer_I_tmp[i]=-selected_rfpower_amplitude;
				if(FPGA_Audio_Buffer_Q_tmp[i]<-selected_rfpower_amplitude) FPGA_Audio_Buffer_Q_tmp[i]=-selected_rfpower_amplitude;
			}
		}
	}

	if (TRX.Mute && !TRX_tune)
	{
		arm_scale_f32(FPGA_Audio_Buffer_I_tmp, 0, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_scale_f32(FPGA_Audio_Buffer_Q_tmp, 0, FPGA_Audio_Buffer_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}

	//Send TX data to FFT
	for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		if (FFTInputBufferInProgress) // A buffer in progress
		{
			FFTInput_A[FFT_buff_index] = FPGA_Audio_Buffer_I_tmp[i];
			FFTInput_A[FFT_buff_index+1] = FPGA_Audio_Buffer_Q_tmp[i];
		}
		else // B buffer in progress
		{
			FFTInput_B[FFT_buff_index] = FPGA_Audio_Buffer_I_tmp[i];
			FFTInput_B[FFT_buff_index+1] = FPGA_Audio_Buffer_Q_tmp[i];
		}
		FFT_buff_index += 2;
		if (FFT_buff_index == FFT_SIZE * 2)
		{
			FFT_buff_index = 0;
			FFTInputBufferInProgress = !FFTInputBufferInProgress;
		}
	}
	
	//Loopback mode
	if (TRX_getMode() == TRX_MODE_LOOPBACK && !TRX_tune)
	{
		//OUT Volume
		arm_scale_f32(FPGA_Audio_Buffer_I_tmp, (float32_t)TRX.Volume / 50.0f, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);

		for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			Processor_AudioBuffer_A[i * 2] = FPGA_Audio_Buffer_I_tmp[i]; //left channel 
			Processor_AudioBuffer_A[i * 2 + 1] = Processor_AudioBuffer_A[i * 2]; //right channel 
		}

		if (WM8731_DMA_state) //compleate
		{
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[FPGA_AUDIO_BUFFER_SIZE], FPGA_AUDIO_BUFFER_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			AUDIOPROC_TXA_samples++;
		}
		else //half
		{
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[0], FPGA_AUDIO_BUFFER_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			AUDIOPROC_TXB_samples++;
		}
	}
	else
	{
		if (FPGA_Audio_Buffer_State) //Send to FPGA DMA
		{
			AUDIOPROC_TXA_samples++;
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&FPGA_Audio_Buffer_I_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_I[FPGA_AUDIO_BUFFER_HALF_SIZE], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&FPGA_Audio_Buffer_Q_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_Q[FPGA_AUDIO_BUFFER_HALF_SIZE], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		}
		else
		{
			AUDIOPROC_TXB_samples++;
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&FPGA_Audio_Buffer_I_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_I[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&FPGA_Audio_Buffer_Q_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_Q[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		}
	}
	Processor_NeedTXBuffer = false;
	Processor_NeedRXBuffer = false;
}

void processRxAudio(void)
{
	if (!Processor_NeedRXBuffer) return;
	AUDIOPROC_samples++;
	readHalfFromCircleBuffer32((float32_t *)&FPGA_Audio_Buffer_Q[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], FPGA_Audio_Buffer_Index, FPGA_AUDIO_BUFFER_SIZE);
	readHalfFromCircleBuffer32((float32_t *)&FPGA_Audio_Buffer_I[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], FPGA_Audio_Buffer_Index, FPGA_AUDIO_BUFFER_SIZE);

	//RF Gain
	arm_scale_f32(FPGA_Audio_Buffer_I_tmp, TRX.RF_Gain, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	arm_scale_f32(FPGA_Audio_Buffer_Q_tmp, TRX.RF_Gain, FPGA_Audio_Buffer_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);

	if (TRX_getMode() != TRX_MODE_IQ && TRX_getMode() != TRX_MODE_LOOPBACK)
	{
		//hilbert fir
		if (TRX_getMode() != TRX_MODE_AM && TRX_getMode() != TRX_MODE_NFM && TRX_getMode() != TRX_MODE_WFM)
		{
			for (block = 0; block < numBlocks; block++)
			{
				arm_fir_f32(&FIR_RX_Hilbert_I, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE);
				arm_fir_f32(&FIR_RX_Hilbert_Q, (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE); 
			}
		}
		//IIR LPF
		if (CurrentVFO()->Filter_Width > 0)
			for (block = 0; block < numBlocks; block++)
			{
				arm_iir_lattice_f32(&IIR_LPF_I, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_I_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE);
				arm_iir_lattice_f32(&IIR_LPF_Q, (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0] + (block*APROCESSOR_BLOCK_SIZE), APROCESSOR_BLOCK_SIZE); 
			}
		switch (TRX_getMode())
		{
		case TRX_MODE_LSB:
		case TRX_MODE_DIGI_L:
		case TRX_MODE_CW_L:
			arm_sub_f32((float32_t *)&FPGA_Audio_Buffer_I_tmp[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);   // difference of I and Q - LSB
			break;
		case TRX_MODE_USB:
		case TRX_MODE_DIGI_U:
		case TRX_MODE_CW_U:
			arm_add_f32((float32_t *)&FPGA_Audio_Buffer_I_tmp[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);   // sum of I and Q - USB
			break;
		case TRX_MODE_AM:
			arm_mult_f32((float32_t *)&FPGA_Audio_Buffer_I_tmp[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_mult_f32((float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_add_f32((float32_t *)&FPGA_Audio_Buffer_I_tmp[0], (float32_t *)&FPGA_Audio_Buffer_Q_tmp[0], (float32_t *)&FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			for (int i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
				arm_sqrt_f32(FPGA_Audio_Buffer_I_tmp[i], &FPGA_Audio_Buffer_I_tmp[i]);
			break;
		case TRX_MODE_NFM:
		case TRX_MODE_WFM:
			DemodulateFM();
			break;
		}
		
		//Prepare data to calculate s-meter
		for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			if(FPGA_Audio_Buffer_I_tmp[i]>Processor_RX_Audio_Samples_MAX_value) Processor_RX_Audio_Samples_MAX_value=FPGA_Audio_Buffer_I_tmp[i];
			if(FPGA_Audio_Buffer_Q_tmp[i]>Processor_RX_Audio_Samples_MAX_value) Processor_RX_Audio_Samples_MAX_value=FPGA_Audio_Buffer_Q_tmp[i];
			if(FPGA_Audio_Buffer_I_tmp[i]<Processor_RX_Audio_Samples_MIN_value) Processor_RX_Audio_Samples_MIN_value=FPGA_Audio_Buffer_I_tmp[i];
			if(FPGA_Audio_Buffer_Q_tmp[i]<Processor_RX_Audio_Samples_MIN_value) Processor_RX_Audio_Samples_MIN_value=FPGA_Audio_Buffer_Q_tmp[i];
		}
		
		//AGC
		if (CurrentVFO()->Agc && TRX_getMode() != TRX_MODE_NFM && TRX_getMode() != TRX_MODE_WFM)
			RxAgcWdsp(numBlocks*APROCESSOR_BLOCK_SIZE, (float32_t *)&FPGA_Audio_Buffer_I_tmp[0]); //AGC
		//
		memcpy(&FPGA_Audio_Buffer_Q_tmp[0], &FPGA_Audio_Buffer_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE * 4); //double channel
	}

	//OUT Volume
	arm_scale_f32(FPGA_Audio_Buffer_I_tmp, (float32_t)TRX.Volume / 100.0f, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	arm_scale_f32(FPGA_Audio_Buffer_Q_tmp, (float32_t)TRX.Volume / 100.0f, FPGA_Audio_Buffer_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	if (TRX.Mute)
	{
		arm_scale_f32(FPGA_Audio_Buffer_I_tmp, 0, FPGA_Audio_Buffer_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_scale_f32(FPGA_Audio_Buffer_Q_tmp, 0, FPGA_Audio_Buffer_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}

	//Prepare data to DMA
	if (Processor_AudioBuffer_ReadyBuffer == 0)
	{
		for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			Processor_AudioBuffer_B[i * 2] = FPGA_Audio_Buffer_I_tmp[i]; //left channel
			Processor_AudioBuffer_B[i * 2 + 1] = FPGA_Audio_Buffer_Q_tmp[i]; //right channel
		}
		Processor_AudioBuffer_ReadyBuffer = 1;
	}
	else
	{
		for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			Processor_AudioBuffer_A[i * 2] = FPGA_Audio_Buffer_I_tmp[i]; //left channel
			Processor_AudioBuffer_A[i * 2 + 1] = FPGA_Audio_Buffer_Q_tmp[i]; //right channel
		}
		Processor_AudioBuffer_ReadyBuffer = 0;
	}
	//Send to Codec DMA
	if (WM8731_DMA_state) //complete
	{
		if (Processor_AudioBuffer_ReadyBuffer == 0)
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[FPGA_AUDIO_BUFFER_SIZE], FPGA_AUDIO_BUFFER_SIZE);
		else
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&Processor_AudioBuffer_B[0], (uint32_t)&CODEC_Audio_Buffer_RX[FPGA_AUDIO_BUFFER_SIZE], FPGA_AUDIO_BUFFER_SIZE);
		HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		AUDIOPROC_TXA_samples++;
	}
	else //half
	{
		if (Processor_AudioBuffer_ReadyBuffer == 0)
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[0], FPGA_AUDIO_BUFFER_SIZE);
		else
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&Processor_AudioBuffer_B[0], (uint32_t)&CODEC_Audio_Buffer_RX[0], FPGA_AUDIO_BUFFER_SIZE);
		HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		AUDIOPROC_TXB_samples++;
	}
	Processor_NeedRXBuffer = false;
}

static void DemodulateFM(void)
{
	float32_t angle, x, y, a, b;
	static float lpf_prev, hpf_prev_a, hpf_prev_b;// used in FM detection and low/high pass processing
	float32_t squelch_buf[FPGA_AUDIO_BUFFER_HALF_SIZE];

	for (uint16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		// first, calculate "x" and "y" for the arctan2, comparing the vectors of present data with previous data
		y = (FPGA_Audio_Buffer_Q_tmp[i] * i_prev) - (FPGA_Audio_Buffer_I_tmp[i] * q_prev);
		x = (FPGA_Audio_Buffer_I_tmp[i] * i_prev) + (FPGA_Audio_Buffer_Q_tmp[i] * q_prev);
		angle = atan2f(y, x);

		// we now have our audio in "angle"
		squelch_buf[i] = angle;	// save audio in "d" buffer for squelch noise filtering/detection - done later

		a = lpf_prev + (FM_RX_LPF_ALPHA * (angle - lpf_prev));	//
		lpf_prev = a;			// save "[n-1]" sample for next iteration

		q_prev = FPGA_Audio_Buffer_Q_tmp[i];// save "previous" value of each channel to allow detection of the change of angle in next go-around
		i_prev = FPGA_Audio_Buffer_I_tmp[i];

		if ((!TRX_squelched) || (!TRX.FM_SQL_threshold)) // high-pass audio only if we are un-squelched (to save processor time)
		{
			if (TRX_getMode() == TRX_MODE_WFM)
			{
				FPGA_Audio_Buffer_I_tmp[i] = (float32_t)(angle / PI * (1 << 14)); //second way
			}
			else
			{
				b = FM_RX_HPF_ALPHA * (hpf_prev_b + a - hpf_prev_a);// do differentiation
				hpf_prev_a = a;		// save "[n-1]" samples for next iteration
				hpf_prev_b = b;
				FPGA_Audio_Buffer_I_tmp[i] = b * 30000.0f;// save demodulated and filtered audio in main audio processing buffer
			}
		}
		else if (TRX_squelched)// were we squelched or tone NOT detected?
			FPGA_Audio_Buffer_I_tmp[i] = 0;// do not filter receive audio - fill buffer with zeroes to mute it
	}

	// *** Squelch Processing ***
	//arm_iir_lattice_f32(&IIR_Squelch_HPF, squelch_buf, squelch_buf, FPGA_AUDIO_BUFFER_HALF_SIZE);	// Do IIR high-pass filter on audio so we may detect squelch noise energy
	fm_sql_avg = ((1 - FM_RX_SQL_SMOOTHING) * fm_sql_avg) + (FM_RX_SQL_SMOOTHING * sqrtf(fabsf(squelch_buf[0])));// IIR filter squelch energy magnitude:  We need look at only one representative sample

	// Squelch processing
	// Determine if the (averaged) energy in "ads.fm_sql_avg" is above or below the squelch threshold
	if (fm_sql_count == 0)	// do the squelch threshold calculation much less often than we are called to process this audio
	{
		if (fm_sql_avg > 0.7f)	// limit maximum noise value in averaging to keep it from going out into the weeds under no-signal conditions (higher = noisier)
			fm_sql_avg = 0.7f;
		b = fm_sql_avg * 10;// scale noise amplitude to range of squelch setting
		// Now evaluate noise power with respect to squelch setting
		if (!TRX.FM_SQL_threshold)	 	// is squelch set to zero?
			TRX_squelched = false;		// yes, the we are un-squelched
		else if (TRX_squelched)	 	// are we squelched?
		{
			if (b <= (float)((10-TRX.FM_SQL_threshold) - FM_SQUELCH_HYSTERESIS))	// yes - is average above threshold plus hysteresis?
				TRX_squelched = false;		//  yes, open the squelch
		}
		else	 	// is the squelch open (e.g. passing audio)?
		{
			if ((10-TRX.FM_SQL_threshold) > FM_SQUELCH_HYSTERESIS)// is setting higher than hysteresis?
			{
				if (b > (float)((10-TRX.FM_SQL_threshold) + FM_SQUELCH_HYSTERESIS))// yes - is average below threshold minus hysteresis?
					TRX_squelched = true;	// yes, close the squelch
			}
			else	 // setting is lower than hysteresis so we can't use it!
			{
				if (b > (10-(float)TRX.FM_SQL_threshold))// yes - is average below threshold?
					TRX_squelched = true;	// yes, close the squelch
			}
		}
		//
		fm_sql_count++;// bump count that controls how often the squelch threshold is checked
		fm_sql_count &= FM_SQUELCH_PROC_DECIMATION;	// enforce the count limit
	}
}

static void ModulateFM()
{
	static uint16_t modulation=1024;
  static float32_t hpf_prev_a=0;
	static float32_t hpf_prev_b=0;
	static float32_t sin_data=0;
  static uint32_t fm_mod_accum = 0;
  // Do differentiating high-pass filter to provide 6dB/octave pre-emphasis - which also removes any DC component!
  for(int i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
  {
    float32_t a = FPGA_Audio_Buffer_I_tmp[i];
    hpf_prev_b = FM_TX_HPF_ALPHA * (hpf_prev_b + a - hpf_prev_a);    // do differentiation
    hpf_prev_a = a;     // save "[n-1] samples for next iteration
    fm_mod_accum    += hpf_prev_b;   // save differentiated data in audio buffer // change frequency using scaled audio
    fm_mod_accum    %= modulation;             // limit range
		sin_data=(fm_mod_accum/(float32_t)modulation)*PI;
    FPGA_Audio_Buffer_I_tmp[i] = selected_rfpower_amplitude*arm_sin_f32(sin_data);
		FPGA_Audio_Buffer_Q_tmp[i] = selected_rfpower_amplitude*arm_sin_f32(sin_data-(PI/4));
  }
}
