/*############################################################################
  # Copyright (C) Intel Corporation
  #
  # SPDX-License-Identifier: MIT
  ############################################################################*/

#include <string>
#include "./vpl-common.h"

#include "vpl/mfxvideo.h"

#define MAX_LENGTH             260
#define MAX_WIDTH              3840
#define MAX_HEIGHT             2160
#define MAX_BS_BUFFER_SIZE     64 * 1024 * 1024
#define DEFAULT_BS_BUFFER_SIZE 2 * 1024 * 1024

#define IS_ARG_EQ(a, b) (!strcmp((a), (b)))
mfxU32 repeatCount = 0;

mfxStatus AllocateExternalMemorySurface(std::vector<mfxU8>* dec_buf,
                                        mfxFrameSurface1* surfpool,
                                        mfxFrameInfo* frame_info,
                                        mfxU16 surfnum);
mfxStatus ReadEncodedStream(mfxBitstream& bs, mfxU32 codecid, FILE* f, mfxU32 repeat);
void WriteRawFrame(mfxFrameSurface1* pSurface, FILE* f);
mfxU32 GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height);
int GetFreeSurfaceIndex(mfxFrameSurface1* SurfacesPool, mfxU16 nPoolSize);
char** ValidateInput(int cnt, char* in[]);
void str_upper(char* str, int l);
char* ValidateFileName(char* in);
bool ValidateSize(char* in, mfxU32* vsize, mfxU32 vmax);
bool ValidateParams(Params* params);
bool ParseArgsAndValidate(int argc, char* argv[], Params* params);
void Usage(void);
mfxStatus InitializeSession(Params* params, mfxSession* session);
void PrintDecParams(mfxVideoParam* mfxDecParams);
void PrintFrameParams(mfxFrameSurface1* frame);

// A callback function which is triggered by mfxFrameSurfaceInterface::OnComplete()
// when decoded frame is ready
static void cb_OnComplete(mfxStatus sts) {
    // printf("%d\n", sts);
    return;
}

typedef void (*VplCallback)(mfxStatus sts);
VplCallback on_complete;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        Usage();
        return 1; // return 1 as error code
    }

    char** cmd_args;
    cmd_args = ValidateInput(argc, argv);
    if (cmd_args == NULL) {
        Usage();
        return 1; // return 1 as error code
    }

    Params params = { 0 };
    if (ParseArgsAndValidate(argc, cmd_args, &params) == false) {
        Usage();
        return 1; // return 1 as error code
    }

    printf("opening %s\n", params.infileName);

    FILE* fSource = fopen(params.infileName, "rb");
    if (!fSource) {
        printf("could not open input file, %s\n", params.infileName);
        return 1;
    }

    FILE* fSink = fopen(params.outfileName, "wb");
    if (!fSink) {
        fclose(fSource);
        printf("could not create output file, %s\n", params.outfileName);
        return 1;
    }

    on_complete = cb_OnComplete;

    mfxStatus sts      = MFX_ERR_NOT_INITIALIZED;
    mfxSession session = nullptr;

    sts = InitializeSession(&params, &session);
    if (sts != MFX_ERR_NONE) {
        fclose(fSource);
        fclose(fSink);
        puts("MFXInitEx error.  Could not initialize session");
        return sts;
    }

    printf("Dispatcher mode = %s\n", DispatcherModeString[params.dispatcherMode]);
    printf("Memory mode     = %s\n", MemoryModeString[params.memoryMode]);

    mfxIMPL impl;
    MFXQueryIMPL(session, &impl);
    printf("Implementation  = %s\n", (MFX_IMPL_SOFTWARE == impl) ? "SOFTWARE" : "HARDWARE");

    mfxVersion ver;
    MFXQueryVersion(session, &ver);
    printf("API version     = %d.%d\n", ver.Major, ver.Minor);

    puts("library initialized");

    // prepare input bitstream
    mfxBitstream mfxBS = { 0 };
    if (params.srcbsbufSize > 0 && params.srcbsbufSize <= MAX_BS_BUFFER_SIZE)
        mfxBS.MaxLength = params.srcbsbufSize;
    else {
        fclose(fSource);
        fclose(fSink);
        puts("Memory allocation error. Bitstream buffersize is not correct");
        return 1;
    }

    std::vector<mfxU8> input_buffer;
    input_buffer.resize(mfxBS.MaxLength);
    mfxBS.Data = input_buffer.data();

    ReadEncodedStream(mfxBS, params.srcFourCC, fSource, params.repeat);

    // initialize decode parameters from stream header
    mfxVideoParam mfxDecParams = { 0 };

    // do lazy-init for AUTO mode
    if (params.memoryMode == MEM_MODE_AUTO) {
        mfxBS.CodecId = params.srcFourCC;
    }
    else {
        // initialize decode parameters from stream header
        memset(&mfxDecParams, 0, sizeof(mfxDecParams));
        mfxDecParams.mfx.CodecId = params.srcFourCC;
        mfxDecParams.IOPattern   = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        sts                      = MFXVideoDECODE_DecodeHeader(session, &mfxBS, &mfxDecParams);

        // disable film-grain denoise
        if (params.filmGrain == 0) {
            mfxDecParams.mfx.FilmGrain = 0;
        }

        //only MFX_CHROMAFORMAT_YUV420 in I420 and I010 colorspaces allowed
        switch (mfxDecParams.mfx.FrameInfo.FourCC) {
            case MFX_FOURCC_I420:
            case MFX_FOURCC_NV12:
                if (mfxDecParams.mfx.FrameInfo.BitDepthLuma &&
                    (mfxDecParams.mfx.FrameInfo.BitDepthLuma != 8)) {
                    printf("Unsupported Luma Bit Depth for I420\n");
                    fclose(fSource);
                    fclose(fSink);
                    return 1;
                }

                if (mfxDecParams.mfx.FrameInfo.ChromaFormat &&
                    (mfxDecParams.mfx.FrameInfo.ChromaFormat != MFX_CHROMAFORMAT_YUV420)) {
                    printf("Unsupported chroma format for I420\n");
                    fclose(fSource);
                    fclose(fSink);
                    return 1;
                }

                break;
            case MFX_FOURCC_P010:
                mfxDecParams.mfx.FrameInfo.Shift = 1;
            case MFX_FOURCC_I010:
                if (mfxDecParams.mfx.FrameInfo.BitDepthLuma &&
                    (mfxDecParams.mfx.FrameInfo.BitDepthLuma != 10)) {
                    printf("Unsupported Luma Bit Depth for I010\n");
                    fclose(fSource);
                    fclose(fSink);
                    return 1;
                }

                if (mfxDecParams.mfx.FrameInfo.ChromaFormat &&
                    (mfxDecParams.mfx.FrameInfo.ChromaFormat != MFX_CHROMAFORMAT_YUV420)) {
                    printf("Unsupported chroma format for I010\n");
                    fclose(fSource);
                    fclose(fSink);
                    return 1;
                }
                break;
            default:
                printf("Unsupported FourCC\n");
                fclose(fSource);
                fclose(fSink);
                return 1;
        }

        if (sts != MFX_ERR_NONE) {
            fclose(fSource);
            fclose(fSink);
            printf("Problem decoding header.  DecodeHeader sts=%d\n", sts);
            return 1;
        }

        // input parameters finished, now initialize decode
        sts = MFXVideoDECODE_Init(session, &mfxDecParams);
        if (sts != MFX_ERR_NONE) {
            fclose(fSource);
            fclose(fSink);
            PrintDecParams(&mfxDecParams);
            puts("Could not initialize decode");
            return 1;
        }
    }

    mfxFrameAllocRequest DecRequest = { 0 };
    mfxU16 nSurfNumDec              = 0;
    mfxFrameSurface1* decSurfaces   = nullptr;
    int nIndex                      = -1;
    std::vector<mfxU8> DECoutbuf;

    if (params.memoryMode == MEM_MODE_EXTERNAL) {
        // Query number required surfaces for decoder
        DecRequest = { 0 };
        MFXVideoDECODE_QueryIOSurf(session, &mfxDecParams, &DecRequest);

        // Determine the required number of surfaces for decoder output
        nSurfNumDec = DecRequest.NumFrameSuggested;

        decSurfaces = new mfxFrameSurface1[nSurfNumDec];
        sts         = AllocateExternalMemorySurface(&DECoutbuf,
                                            decSurfaces,
                                            &mfxDecParams.mfx.FrameInfo,
                                            nSurfNumDec);
        if (sts != MFX_ERR_NONE) {
            if (decSurfaces)
                delete[] decSurfaces;
            fclose(fSource);
            fclose(fSink);
            puts("External memory allocation error.");
            return sts;
        }
    }

    // ------------------
    // main loop
    // ------------------
    int framenum                      = 0;
    double decode_time                = 0;
    double sync_time                  = 0;
    mfxSyncPoint syncp                = { 0 };
    mfxFrameSurface1* pmfxWorkSurface = nullptr;
    mfxFrameSurface1* pmfxOutSurface  = nullptr;

    puts("start decoding");
    bool isdraining = false;
    for (;;) {
        bool stillgoing = true;

        if (params.memoryMode == MEM_MODE_EXTERNAL) {
            nIndex = GetFreeSurfaceIndex(decSurfaces, nSurfNumDec);
        }

        pmfxWorkSurface = nullptr;
        while (stillgoing) {
            // submit async decode request
            auto t0 = std::chrono::high_resolution_clock::now();
            if (params.memoryMode == MEM_MODE_EXTERNAL) {
                pmfxWorkSurface = &decSurfaces[nIndex];
            }
            else if (params.memoryMode == MEM_MODE_INTERNAL) {
                // if previous call to DecodeFrameAsync() returned a non-fatal error
                //   (e.g. MFX_ERR_MORE_DATA = needs more bitstream data) then send the same work surface again
                if (!pmfxWorkSurface) {
                    sts = MFXMemory_GetSurfaceForDecode(session, &pmfxWorkSurface);
                    if (sts) {
                        printf("Error in GetSurfaceForDecode: sts=%d\n", sts);
                        exit(1);
                    }
                    pmfxWorkSurface->FrameInterface->OnComplete = on_complete;
                }
            }
            else if (params.memoryMode == MEM_MODE_AUTO) {
                pmfxWorkSurface = nullptr;
            }

            sts = MFXVideoDECODE_DecodeFrameAsync(session,
                                                  (isdraining ? nullptr : &mfxBS),
                                                  pmfxWorkSurface,
                                                  &pmfxOutSurface,
                                                  &syncp);

            auto t1 = std::chrono::high_resolution_clock::now();
            decode_time += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

            // next step actions provided by application
            switch (sts) {
                case MFX_ERR_MORE_DATA: // more data is needed to decode
                    ReadEncodedStream(mfxBS, params.srcFourCC, fSource, params.repeat);
                    if (mfxBS.DataLength == 0) {
                        if (isdraining == true) {
                            stillgoing = false; // stop if end of file and all drained
                        }
                        else {
                            isdraining = true;
                        }
                    }
                    break;
                case MFX_ERR_MORE_SURFACE: // feed a fresh surface to decode
                    if (params.memoryMode == MEM_MODE_EXTERNAL) {
                        nIndex = GetFreeSurfaceIndex(decSurfaces, nSurfNumDec);
                    }
                    else {
                        printf(
                            "Error - MFX_ERR_MORE_SURFACE returned with internal memory allocation\n");
                        exit(1);
                    }
                    break;
                case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
                    if (params.memoryMode == MEM_MODE_EXTERNAL) {
                        MFXVideoDECODE_GetVideoParam(session, &mfxDecParams);
                        sts = AllocateExternalMemorySurface(&DECoutbuf,
                                                            decSurfaces,
                                                            &mfxDecParams.mfx.FrameInfo,
                                                            nSurfNumDec);
                        if (sts != MFX_ERR_NONE) {
                            if (fSource) {
                                fclose(fSource);
                            }
                            if (fSink) {
                                fclose(fSink);
                            }
                            puts("External memory allocation error after resolution change.");
                            return sts;
                        }

                        nIndex          = GetFreeSurfaceIndex(decSurfaces, nSurfNumDec);
                        pmfxWorkSurface = &decSurfaces[nIndex];

                        sts = MFXVideoDECODE_DecodeFrameAsync(session,
                                                              nullptr,
                                                              pmfxWorkSurface,
                                                              &pmfxOutSurface,
                                                              &syncp);

                        stillgoing = false;
                        sts        = MFX_ERR_NONE;
                        break;
                    }
                    else {
                        printf("Error in DecodeFrameAsync: sts=%d\n", sts);
                        exit(1);
                        break;
                    }
                    break;
                case MFX_ERR_NONE: // no more steps needed, exit loop
                    stillgoing = false;
                    break;
                default: // state is not one of the cases above
                    printf("Error in DecodeFrameAsync: sts=%d\n", sts);
                    exit(1);
                    break;
            }
        }

        if (sts < 0)
            break;

        if (params.verboseMode) {
            if (framenum == 0) {
                mfxVideoParam tmpParams = { 0 };

                //Show settable parameters
                MFXVideoDECODE_Query(session, nullptr, &tmpParams);

                puts("-----------------------");
                puts("Settable Parameters (nonzero=settable):");
                PrintDecParams(&tmpParams);
                puts("-----------------------");

                //Show parameters from input video
                tmpParams = { 0 };
                MFXVideoDECODE_GetVideoParam(session, &tmpParams);

                puts("-----------------------");
                puts("Current Parameters:");
                PrintDecParams(&tmpParams);
                puts("-----------------------");
            }

            puts("-----------------------");
            puts("mfxFrameSurface1 parameters:");
            PrintFrameParams(pmfxOutSurface);
            puts("-----------------------");
        }

        // data available to app only after sync
        auto t0 = std::chrono::high_resolution_clock::now();
        MFXVideoCORE_SyncOperation(session, syncp, 60000);
        auto t1 = std::chrono::high_resolution_clock::now();
        sync_time += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        if (params.memoryMode == MEM_MODE_INTERNAL || params.memoryMode == MEM_MODE_AUTO) {
            pmfxOutSurface->FrameInterface->Map(pmfxOutSurface, MFX_MAP_READ);
        }

        // write output if output file specified
        if (fSink) {
            // this is only for mult-res stream test case
            if (params.outWidth != 0 && params.outHeight != 0) {
                if (pmfxOutSurface->Info.Width == params.outWidth &&
                    pmfxOutSurface->Info.Height == params.outHeight) {
                    WriteRawFrame(pmfxOutSurface, fSink);
                }
            }
            else {
                WriteRawFrame(pmfxOutSurface, fSink);
            }
        }

        if (params.memoryMode == MEM_MODE_INTERNAL || params.memoryMode == MEM_MODE_AUTO) {
            pmfxOutSurface->FrameInterface->Unmap(pmfxOutSurface);
            pmfxOutSurface->FrameInterface->Release(pmfxOutSurface);
        }

        framenum++;
        if (params.maxFrames && framenum >= static_cast<int>(params.maxFrames))
            break;
    }

    printf("read %d frames\n", framenum);
    if (framenum) {
        printf("decode avg=%f usec, sync avg=%f usec\n",
               decode_time / framenum,
               sync_time / framenum);
    }

    if (fSink) {
        fclose(fSink);
    }
    if (fSource) {
        fclose(fSource);
    }

    if (params.memoryMode == MEM_MODE_EXTERNAL) {
        if (decSurfaces) {
            delete[] decSurfaces;
        }
    }

    MFXVideoDECODE_Close(session);
    MFXClose(session);

    if (params.dispatcherMode == DISPATCHER_MODE_VPL_20)
        CloseNewDispatcher();

    return 0;
}

mfxStatus AllocateExternalMemorySurface(std::vector<mfxU8>* dec_buf,
                                        mfxFrameSurface1* surfpool,
                                        mfxFrameInfo* frame_info,
                                        mfxU16 surfnum) {
    // initialize surface pool for decode (I420 format)
    mfxU32 surfaceSize = GetSurfaceSize(frame_info->FourCC, frame_info->Width, frame_info->Height);
    if (!surfaceSize)
        return MFX_ERR_MEMORY_ALLOC;

    size_t framePoolBufSize = static_cast<size_t>(surfaceSize) * surfnum;
    dec_buf->resize(framePoolBufSize);
    mfxU8* decout = dec_buf->data();

    mfxU16 surfW = (frame_info->FourCC == MFX_FOURCC_I010 || frame_info->FourCC == MFX_FOURCC_P010)
                       ? frame_info->Width * 2
                       : frame_info->Width;
    mfxU16 surfH = frame_info->Height;

    for (mfxU32 i = 0; i < surfnum; i++) {
        surfpool[i]            = { 0 };
        surfpool[i].Info       = *frame_info;
        size_t buf_offset      = static_cast<size_t>(i) * surfaceSize;
        surfpool[i].Data.Y     = decout + buf_offset;
        surfpool[i].Data.U     = decout + buf_offset + (surfW * surfH);
        surfpool[i].Data.V     = surfpool[i].Data.U + ((surfW / 2) * (surfH / 2));
        surfpool[i].Data.Pitch = surfW;
    }

    return MFX_ERR_NONE;
}

mfxU32 GetSurfaceSize(mfxU32 FourCC, mfxU32 width, mfxU32 height) {
    mfxU32 nbytes = 0;

    switch (FourCC) {
        case MFX_FOURCC_NV12:
        case MFX_FOURCC_I420:
            nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
            break;
        case MFX_FOURCC_P010:
        case MFX_FOURCC_I010:
            nbytes = width * height + (width >> 1) * (height >> 1) + (width >> 1) * (height >> 1);
            nbytes *= 2;
            break;
        default:
            break;
    }

    return nbytes;
}

int GetFreeSurfaceIndex(mfxFrameSurface1* SurfacesPool, mfxU16 nPoolSize) {
    for (mfxU16 i = 0; i < nPoolSize; i++) {
        if (0 == SurfacesPool[i].Data.Locked)
            return i;
    }
    return MFX_ERR_NOT_FOUND;
}

mfxStatus ReadEncodedStream(mfxBitstream& bs, mfxU32 codecid, FILE* f, mfxU32 repeat) {
    memmove(bs.Data, bs.Data + bs.DataOffset, bs.DataLength);
    bs.DataOffset = 0;

    if (codecid == MFX_CODEC_AV1) {
        // spec for IVF headers
        // https://wiki.multimedia.cx/index.php/IVF

        // extract AV1 elementary stream from IVF file.
        // should remove stream header and frame header.
        // and use the frame size information to read AV1 frame.
        mfxU32 nBytesInFrame = 0;
        mfxU64 nTimeStamp    = 0;
        mfxU32 nBytesRead    = 0;
        static bool bread_streamheader;

        // read stream header, only once at the beginning, 32 bytes
        if (bread_streamheader == false) {
            mfxU8 header[32] = { 0 };
            nBytesRead       = (mfxU32)fread(header, 1, 32, f);
            if (nBytesRead == 0)
                return MFX_ERR_MORE_DATA;
            bread_streamheader = true;
        }

        bs.DataLength = 0;

        // read frame header and parse frame data
        while (!feof(f)) {
            nBytesRead = (mfxU32)fread(&nBytesInFrame, 1, 4, f);
            if (nBytesInFrame == 0 || nBytesInFrame > bs.MaxLength)
                return MFX_ERR_ABORTED;
            if (nBytesRead == 0)
                return MFX_ERR_MORE_DATA;

            // check whether buffer is over if we read this frame or not
            if (bs.DataLength + nBytesInFrame > bs.MaxLength) {
                // set file pointer location back to -4
                // so, the access pointer for next frame will be the "bytesinframe" location and break
                fseek(f, -4, SEEK_CUR);
                break;
            }

            nBytesRead = (mfxU32)fread(&nTimeStamp, 1, 8, f);
            if (nBytesRead == 0)
                return MFX_ERR_MORE_DATA;

            nBytesRead = (mfxU32)fread(bs.Data + bs.DataLength, 1, nBytesInFrame, f);
            if (nBytesRead == 0)
                return MFX_ERR_MORE_DATA;

            bs.DataLength += nBytesRead;
        }
    }
    else {
        bs.DataLength +=
            static_cast<mfxU32>(fread(bs.Data + bs.DataLength, 1, bs.MaxLength - bs.DataLength, f));

        while (feof(f) && repeat > 0 && repeatCount <= repeat) {
            if (repeatCount == repeat)
                return MFX_ERR_NONE;

            // The end-of-file and error internal indicators associated to the stream are cleared after a successful call to this function,
            // and all effects from previous calls to ungetc on this stream are dropped.
            rewind(f);

            repeatCount++;
        }
    }
    return MFX_ERR_NONE;
}

void WriteRawFrame(mfxFrameSurface1* pSurface, FILE* f) {
    mfxU16 w, h, i, pitch;
    mfxFrameInfo* pInfo = &pSurface->Info;
    mfxFrameData* pData = &pSurface->Data;

    w = pInfo->CropW;
    h = pInfo->CropH;

    // write the output to disk
    switch (pInfo->FourCC) {
        case MFX_FOURCC_NV12:
            //Y
            pitch = pData->Pitch;
            for (i = 0; i < h; i++) {
                fwrite(pData->Y + i * pitch, 1, w, f);
            }

            //UV
            for (i = 0; i < h / 2; i++) {
                fwrite(pData->UV + i * pitch, 1, w, f);
            }

            break;
        case MFX_FOURCC_I420:
            //Y
            pitch = pData->Pitch;
            for (i = 0; i < h; i++) {
                fwrite(pData->Y + i * pitch, 1, w, f);
            }

            //U
            pitch /= 2;
            h /= 2;
            w /= 2;
            for (i = 0; i < h; i++) {
                fwrite(pData->U + i * pitch, 1, w, f);
            }
            //V
            for (i = 0; i < h; i++) {
                fwrite(pData->V + i * pitch, 1, w, f);
            }
            break;

        case MFX_FOURCC_P010:
            //Y
            pitch = pData->Pitch;
            w *= 2;
            for (i = 0; i < h; i++) {
                fwrite(pData->Y + i * pitch, 1, w, f);
            }

            //UV
            for (i = 0; i < h / 2; i++) {
                fwrite(pData->UV + i * pitch, 1, w, f);
            }
            break;

        case MFX_FOURCC_I010:
            //Y
            pitch = pData->Pitch;
            w *= 2;
            for (i = 0; i < h; i++) {
                fwrite(pSurface->Data.Y + i * pitch, 1, w, f);
            }

            //U
            pitch /= 2;
            w /= 2;
            h /= 2;
            for (i = 0; i < h; i++) {
                fwrite(pSurface->Data.U + i * pitch, 1, w, f);
            }
            //V
            for (i = 0; i < h; i++) {
                fwrite(pSurface->Data.V + i * pitch, 1, w, f);
            }
            break;
        default:
            break;
    }

    return;
}

char** ValidateInput(int cnt, char* in[]) {
    if (in) {
        for (int i = 0; i < cnt; i++) {
            if (strlen(in[i]) > MAX_LENGTH)
                return NULL;
        }
    }

    return in;
}

void str_upper(char* str, int l) {
    for (int i = 0; i < l; i++) {
        str[i] = static_cast<char>(toupper(str[i]));
    }
}

char* ValidateFileName(char* in) {
    if (in) {
        if (strlen(in) > MAX_LENGTH)
            return NULL;
    }

    return in;
}

bool ValidateSize(char* in, mfxU32* vsize, mfxU32 vmax) {
    if (in) {
        *vsize = static_cast<mfxU16>(strtol(in, NULL, 10));
        if (*vsize <= vmax)
            return true;
    }

    *vsize = 0;
    return false;
}

// perform basic parameter validation and setup
bool ValidateParams(Params* params) {
    // input file (required)
    if (!params->infileName) {
        printf("ERROR - input file name (-i) is required\n");
        return false;
    }

    // output file (required)
    if (!params->outfileName) {
        printf("ERROR - output file name (-i) is required\n");
        return false;
    }

    // input format (required)
    if (!params->infileFormat) {
        printf("ERROR - input format (-if) is required\n");
        return false;
    }

    // input format (required)
    if (strncmp(params->infileFormat, "H264", strlen("H264")) == 0) {
        params->srcFourCC = MFX_CODEC_AVC;
    }
    else if (strncmp(params->infileFormat, "H265", strlen("H265")) == 0) {
        params->srcFourCC = MFX_CODEC_HEVC;
    }
    else if (strncmp(params->infileFormat, "AV1", strlen("AV1")) == 0) {
        params->srcFourCC = MFX_CODEC_AV1;
    }
    else if (strncmp(params->infileFormat, "JPEG", strlen("JPEG")) == 0) {
        params->srcFourCC = MFX_CODEC_JPEG;
    }
    else {
        printf("ERROR - unsupported input format %s\n", params->infileFormat);
        return false;
    }

    // default bitstream buffer size (input)
    if (params->srcbsbufSize == 0) {
        params->srcbsbufSize = DEFAULT_BS_BUFFER_SIZE;
    }

    if (params->memoryMode == MEM_MODE_EXTERNAL) {
        puts("[external memory mode]");
    }
    else if (params->memoryMode == MEM_MODE_INTERNAL) {
        puts("[internal memory mode]");
    }
    else {
        puts("[auto memory mode]");
    }

    // if this param is set, it will be used for decode res-change testing.
    // and will select this resolution from decoded frame to save.
    if (params->outResolution) {
        char* position_ptr = NULL;
        position_ptr       = strchr(params->outResolution, 'X');
        if (position_ptr) {
            if (!ValidateSize(params->outResolution, &params->outWidth, MAX_WIDTH))
                return false;
            position_ptr++;
            if (!ValidateSize(position_ptr, &params->outHeight, MAX_HEIGHT))
                return false;

            printf("only %dx%d res of decoded frames will be saved\n",
                   params->outWidth,
                   params->outHeight);
        }
        else {
            puts("wrong separator for output resolution(-o_res)\n");
            return false;
        }
    }

    return true;
}

bool ParseArgsAndValidate(int argc, char* argv[], Params* params) {
    int idx;
    char* s;

    // init all params to 0
    memset(params, 0, sizeof(Params));

    // set any non-zero defaults
    params->memoryMode     = MEM_MODE_EXTERNAL;
    params->dispatcherMode = DISPATCHER_MODE_LEGACY;
    params->impl           = MFX_IMPL_SOFTWARE;
    params->filmGrain      = -1; // auto

    if (argc < 2)
        return false;

    for (idx = 1; idx < argc;) {
        // all switches must start with '-'
        if (argv[idx][0] != '-') {
            printf("ERROR - invalid argument: %s\n", argv[idx]);
            return false;
        }

        // switch string, starting after the '-'
        s = &argv[idx][1];
        idx++;

        // search for match
        if (IS_ARG_EQ(s, "i")) {
            params->infileName = ValidateFileName(argv[idx++]);
            if (!params->infileName) {
                return false;
            }
        }
        else if (IS_ARG_EQ(s, "o")) {
            params->outfileName = ValidateFileName(argv[idx++]);
            if (!params->outfileName) {
                return false;
            }
        }
        else if (IS_ARG_EQ(s, "n")) {
            params->maxFrames = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "v")) {
            params->verboseMode = true;
        }
        else if (IS_ARG_EQ(s, "if")) {
            params->infileFormat = argv[idx++];
            str_upper(params->infileFormat,
                      static_cast<int>(strlen(params->infileFormat))); // to upper case
        }
        else if (IS_ARG_EQ(s, "of")) {
            params->outfileFormat = argv[idx++];
            str_upper(params->outfileFormat,
                      static_cast<int>(strlen(params->outfileFormat))); // to upper case
        }
        else if (IS_ARG_EQ(s, "sw")) {
            if (!ValidateSize(argv[idx++], &params->srcWidth, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "sh")) {
            if (!ValidateSize(argv[idx++], &params->srcHeight, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "dw")) {
            if (!ValidateSize(argv[idx++], &params->dstWidth, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "dh")) {
            if (!ValidateSize(argv[idx++], &params->dstHeight, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "td")) {
            params->targetDeviceType = argv[idx++];
        }
        else if (IS_ARG_EQ(s, "sbs")) {
            params->srcbsbufSize = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "dbs")) {
            params->dstbsbufSize = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "to")) {
            params->timeout = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "fr")) {
            params->frameRate = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "br")) {
            params->bitRate = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "tu")) {
            params->targetUsage = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "qu")) {
            params->quality = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "bm")) {
            params->brcMode = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "gs")) {
            params->gopSize = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "rp")) {
            if (atoi(argv[idx]) < 0) {
                printf("ERROR - invalid argument: value for -rp switch cannot be negative\n");
                return false;
            }

            params->repeat = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "kd")) {
            params->keyFrameDist = atoi(argv[idx++]);
        }
        else if (IS_ARG_EQ(s, "ci")) {
            params->enableCinterface = 1;
        }
        else if (IS_ARG_EQ(s, "gcm")) {
            params->gpuCopyMode = argv[idx++];
        }
        else if (IS_ARG_EQ(s, "ext")) {
            params->memoryMode = MEM_MODE_EXTERNAL;
        }
        else if (IS_ARG_EQ(s, "int")) {
            params->memoryMode = MEM_MODE_INTERNAL;
        }
        else if (IS_ARG_EQ(s, "auto")) {
            params->memoryMode = MEM_MODE_AUTO;
        }
        else if (IS_ARG_EQ(s, "dsp1")) {
            params->dispatcherMode = DISPATCHER_MODE_LEGACY;
        }
        else if (IS_ARG_EQ(s, "dsp2")) {
            params->dispatcherMode = DISPATCHER_MODE_VPL_20;
        }
        else if (IS_ARG_EQ(s, "scrx")) {
            if (!ValidateSize(argv[idx++], &params->srcCropX, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "scry")) {
            if (!ValidateSize(argv[idx++], &params->srcCropY, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "scrw")) {
            if (!ValidateSize(argv[idx++], &params->srcCropW, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "scrh")) {
            if (!ValidateSize(argv[idx++], &params->srcCropH, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "dcrx")) {
            if (!ValidateSize(argv[idx++], &params->dstCropX, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "dcry")) {
            if (!ValidateSize(argv[idx++], &params->dstCropY, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "dcrw")) {
            if (!ValidateSize(argv[idx++], &params->dstCropW, MAX_WIDTH))
                return false;
        }
        else if (IS_ARG_EQ(s, "dcrh")) {
            if (!ValidateSize(argv[idx++], &params->dstCropH, MAX_HEIGHT))
                return false;
        }
        else if (IS_ARG_EQ(s, "o_res")) {
            params->outResolution = argv[idx++];
            str_upper(params->outResolution,
                      static_cast<int>(strlen(params->outResolution))); // to upper case
        }
        else if (IS_ARG_EQ(s, "fg")) {
            params->filmGrain = atoi(argv[idx++]);
            if (params->filmGrain < 0 || params->filmGrain > 1)
                return false;
        }
        else {
            printf("ERROR - invalid argument: %s\n", argv[idx]);
            return false;
        }
    }

    // run basic parameter validation
    return ValidateParams(params);
}

void Usage(void) {
    printf("\nOptions - Decode:\n");
    printf("  -i     inputFile     ... input file name\n");
    printf("  -o     outputFile    ... output file name\n");
    printf("  -n     maxFrames     ... max frames to decode\n");
    printf("  -if    inputFormat   ... [h264, h265, av1, jpeg]\n");
    printf("  -rp    repeat        ... number of times to repeat decoding\n");
    printf("  -sbs   bsbufSize     ... source bitstream buffer size (bytes)\n");
    printf("  -v     verbose       ... verbose output for debug\n");
    printf("  -fg    filmgrain     ... film-grain denoise (0: disable, 1: enable)\n");
    printf("\nMemory model (default = -ext)\n");
    printf("  -ext  = external memory (1.0 style)\n");
    printf("  -int  = internal memory with MFXMemory_GetSurfaceForDecode\n");
    printf("  -auto = internal memory with NULL working surface + simplified decode path\n");

    printf("\nDispatcher (default = -dsp1)\n");
    printf("  -dsp1 = legacy dispatcher (MSDK 1.x)\n");
    printf("  -dsp2 = smart dispatcher (API %d.%d)\n",
           DEFAULT_VERSION_MAJOR,
           DEFAULT_VERSION_MINOR);

    printf("\nImplementation (default = software\n");

    printf("\nMore for resolution change test\n");
    printf(
        "  -o_res outputRes     ... exclude all frames except those of specified resolution (ex: 128x96)\n");
    printf("\nTo view:\n");
    printf(
        " ffplay -video_size [width]x[height] -pixel_format [pixel format] -f rawvideo [out filename]\n");
    return;
}

mfxStatus InitializeSession(Params* params, mfxSession* session) {
    std::string diagnoseText = "";
    mfxStatus sts            = MFX_ERR_UNDEFINED_BEHAVIOR;

    switch (params->dispatcherMode) {
        case DISPATCHER_MODE_VPL_20:
            diagnoseText = "InitNewDispatcher with WSTYPE_DECODE in DISPATCHER_MODE_VPL_20";
            sts          = InitNewDispatcher(WSTYPE_DECODE, params, session);
            break;
        case DISPATCHER_MODE_LEGACY: {
            if (MFX_IMPL_SOFTWARE == params->impl) {
                diagnoseText = "MFXInitEx with MFX_IMPL_SOFTWARE in DISPATCHER_MODE_LEGACY";
                // initialize session
                mfxInitParam initPar   = { 0 };
                initPar.Version.Major  = DEFAULT_VERSION_MAJOR;
                initPar.Version.Minor  = DEFAULT_VERSION_MINOR;
                initPar.Implementation = MFX_IMPL_SOFTWARE;

                sts = MFXInitEx(initPar, session);
            }
            else {
                diagnoseText = "Tool does not support MFX_IMPL_HARDWARE mode";
                sts          = MFX_ERR_UNSUPPORTED;
            }
        } break;
        default:
            diagnoseText = "Invalid dispatcher mode";
            break;
    }

    if (sts != MFX_ERR_NONE) {
        printf("MFXInitEx error. could not initialize session\n");
        std::cout << "Failed " << diagnoseText.c_str() << std::endl;
        if (sts == MFX_ERR_UNSUPPORTED) {
            printf("Did you configure the necessary environment variables?\n");
        }
        else {
            std::cout << "Error code: " << sts << std::endl;
        }
    }

    return sts;
}

void PrintDecParams(mfxVideoParam* mfxDecParams) {
    std::cout << "AsyncDepth=" << mfxDecParams->AsyncDepth << " | ";
    std::cout << "CodecId=" << mfxDecParams->mfx.CodecId << " | ";
    std::cout << "CodecProfile=" << mfxDecParams->mfx.CodecProfile << " | ";
    std::cout << "CodecLevel=" << mfxDecParams->mfx.CodecLevel << " | ";
    std::cout << "NumThread=" << mfxDecParams->mfx.NumThread << " | ";
    std::cout << "PicStruct=" << mfxDecParams->mfx.FrameInfo.PicStruct << " | ";
    std::cout << "Shift=" << mfxDecParams->mfx.FrameInfo.Shift << " | ";
    std::cout << "BitDepthLuma=" << mfxDecParams->mfx.FrameInfo.BitDepthLuma << " | ";
    std::cout << "BitDepthChroma=" << mfxDecParams->mfx.FrameInfo.BitDepthChroma << " | ";
    std::cout << "ChromaFormat=" << mfxDecParams->mfx.FrameInfo.ChromaFormat << " | ";
    std::cout << "FourCC=" << mfxDecParams->mfx.FrameInfo.FourCC << " | ";
    std::cout << "Width=" << mfxDecParams->mfx.FrameInfo.Width << " | ";
    std::cout << "Height=" << mfxDecParams->mfx.FrameInfo.Height << " | ";
    std::cout << "CropX=" << mfxDecParams->mfx.FrameInfo.CropX << " | ";
    std::cout << "CropY=" << mfxDecParams->mfx.FrameInfo.CropY << " | ";
    std::cout << "CropH=" << mfxDecParams->mfx.FrameInfo.CropH << " | ";
    std::cout << "CropW=" << mfxDecParams->mfx.FrameInfo.CropW << " | ";
    std::cout << "AspectRatioH=" << mfxDecParams->mfx.FrameInfo.AspectRatioH << " | ";
    std::cout << "AspectRatioW=" << mfxDecParams->mfx.FrameInfo.AspectRatioW << " | ";
    std::cout << "FrameRateExtD=" << mfxDecParams->mfx.FrameInfo.FrameRateExtD << " | ";
    std::cout << "FrameRateExtN=" << mfxDecParams->mfx.FrameInfo.FrameRateExtN << " | ";
    std::cout << "DecodedOrder=" << mfxDecParams->mfx.DecodedOrder << " | ";
    if (mfxDecParams->mfx.CodecId == MFX_CODEC_JPEG) {
        std::cout << "Quality=" << mfxDecParams->mfx.Quality << " | ";
        std::cout << "JPEGChromaFormat=" << mfxDecParams->mfx.JPEGChromaFormat << " | ";
        std::cout << "JPEGColorFormat=" << mfxDecParams->mfx.JPEGColorFormat << " | ";
        std::cout << "Rotation=" << mfxDecParams->mfx.Rotation << " | ";
    }
    std::cout << "Protected=" << mfxDecParams->Protected << " | ";
    std::cout << "IOPattern=" << mfxDecParams->IOPattern << " | ";
    std::cout << "NumExtParam=" << mfxDecParams->NumExtParam << " | ";
    std::cout << std::endl;
}

void PrintFrameParams(mfxFrameSurface1* frame) {
    std::cout << "TimeStamp=" << frame->Data.TimeStamp << " | ";
    std::cout << "FrameOrder=" << frame->Data.FrameOrder << " | ";
    std::cout << "Corrupted=" << frame->Data.Corrupted << " | ";
    std::cout << "DataFlag=" << frame->Data.DataFlag << " | ";
    std::cout << std::endl;
}
