#include "AudioSample.h"

namespace SM64 {
/* Sample data is normally never freed: a resource can be unloaded while the
 * audio thread still plays from it. At a full shutdown (re-entry) the audio
 * thread is gone, so the data is freed with its resource. */
bool gAudioSampleFreeData = false;

AudioBankSample* AudioSample::GetPointer() {
    return &mData;
}

size_t AudioSample::GetPointerSize() {
    return sizeof(mData);
}
AudioSample::~AudioSample() {
    /* (mData.sampleAddr may have been moved by the audio loader: free what
     * the factory allocated) */
    if (gAudioSampleFreeData && mOwnedData != nullptr) {
        delete[] mOwnedData;
        mOwnedData = nullptr;
    }
    if (mData.sampleAddr != nullptr) {
        mData.sampleAddr = nullptr;
    }
    if (mData.book->book != nullptr) {
        delete[] mData.book->book;
        mData.book->book = nullptr;
    }
    if (mData.loop->state != nullptr) {
        delete[] mData.loop->state;
        mData.loop->state = nullptr;
    }
}
}