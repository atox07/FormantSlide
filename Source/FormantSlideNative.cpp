    d.midiFullPath = midiPath_;
    d.status = status_;
    d.rootMidi = settings_.firstNote;
    d.lastMidiNote = lastMidiNote_;
    d.lastSlice = lastSlice_;
    d.lastPitchCents = lastPitchCents_;
    return d;
}

std::vector<AudioSlice> FormantSlideNative::sliceSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slices_;
}
bool FormantSlideNative::moveSliceBoundary(int boundaryIndex, int frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (boundaryIndex < 1 || boundaryIndex >= int(slices_.size())) return false;
    auto& left = slices_[size_t(boundaryIndex - 1)];
    auto& right = slices_[size_t(boundaryIndex)];
    // Keep a minimum audible region on either side, and never cross markers.
    const int minGap = std::max(2, wav_.sampleRate / 200); // 5 ms
    const int low = left.begin + minGap;
    const int high = right.end - minGap;
    if (low > high) return false;
    const int boundary = std::clamp(frame, low, high);
    left.end = boundary;
    right.begin = boundary;
    slicesManuallyEdited_ = true;
    // Current playback finishes with its original end until the next note.
    return true;
}

std::vector<int> FormantSlideNative::playingSlices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> active;
    for (const auto& voice : voices_)
        if (voice->releaseFrames != 0 && (voice->samplePlaying_ || voice->drainRemaining_ > 0))
            active.push_back(voice->sliceIndex);
    return active;
}

int FormantSlideNative::wavFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wav_.frames();
}

void _stdcall FormantSlideNative::SaveRestoreState(IStream* stream, BOOL save) {
    if (!stream) return;
    SavedState s;
    if (save) {
        std::lock_guard<std::mutex> lock(mutex_);
        s.equal = settings_.equalSlices;
        s.slices = settings_.sliceCount;
        s.root = settings_.firstNote;
        s.all = settings_.allSlideColors;
        s.glide = settings_.fallbackGlideMs;
        s.pitchOrLegacyOffset = settings_.pitchCents;
        s.offset = settings_.midiOffsetTicks;
        wcsncpy_s(s.wav, wavPath_.c_str(), _TRUNCATE);
        wcsncpy_s(s.midi, midiPath_.c_str(), _TRUNCATE);
        stream->Write(&s, sizeof(s), nullptr);
        SavedSliceMarkerHeader markerHeader;
        markerHeader.count = slicesManuallyEdited_ ? uint32_t(slices_.size()) : 0;
        stream->Write(&markerHeader, sizeof(markerHeader), nullptr);
        if (markerHeader.count > 0) {
            for (const auto& slice : slices_) {
                const int32_t frames[2] = {slice.begin, slice.end};
                stream->Write(frames, sizeof(frames), nullptr);
            }
        }
    } else {
        ULONG read = 0;
        if (FAILED(stream->Read(&s, sizeof(s), &read)) ||
            read < offsetof(SavedState, pitchOrLegacyOffset) ||
            s.magic != 0x46534e32 || (s.version < 1 || s.version > 4)) return;
        Settings restored;
        restored.equalSlices = s.equal != 0;
        restored.sliceCount = int(s.slices);
        restored.allSlideColors = s.all != 0;
        restored.fallbackGlideMs = int(s.glide);
        restored.midiOffsetTicks = s.offset;
        restored.firstNote = std::clamp(int(s.root), 0, 127);
        if (s.version == 2 && read == sizeof(s))
            restored.firstNote = std::clamp(restored.firstNote - std::clamp(s.pitchOrLegacyOffset, -24, 24), 0, 127);
        if (s.version == 4 && read == sizeof(s)) restored.pitchCents = s.pitchOrLegacyOffset;
        s.wav[511] = L'\0';
        s.midi[511] = L'\0';
        updateSettings(restored);
        std::string error;
        if (s.wav[0]) loadWav(s.wav, error);
        if (s.midi[0]) loadMidi(s.midi, error);
        SavedSliceMarkerHeader markerHeader{};
        ULONG headerRead = 0;
        if (SUCCEEDED(stream->Read(&markerHeader, sizeof(markerHeader), &headerRead)) &&
            headerRead == sizeof(markerHeader) && markerHeader.magic == 0x534c4335 &&
            markerHeader.count >= 2 && markerHeader.count <= uint32_t(wavFrameCount() / 2)) {
            std::vector<AudioSlice> restoredSlices;
            for (uint32_t i = 0; i < markerHeader.count; ++i) {
                int32_t bounds[2]{};
                ULONG got = 0;
                if (FAILED(stream->Read(bounds, sizeof(bounds), &got)) || got != sizeof(bounds)) {
                    restoredSlices.clear(); break;
                }
                restoredSlices.push_back({bounds[0], bounds[1]});
            }
            std::lock_guard<std::mutex> lock(mutex_);
            bool valid = restoredSlices.size() == markerHeader.count && wav_.frames() > 0;
            for (size_t i = 0; valid && i < restoredSlices.size(); ++i) {
                valid = restoredSlices[i].begin >= 0 &&
                        restoredSlices[i].end <= wav_.frames() &&
                        restoredSlices[i].begin < restoredSlices[i].end;
                if (i > 0) valid = valid && restoredSlices[i-1].end <= restoredSlices[i].begin;
            }
            if (valid) { slices_ = std::move(restoredSlices); slicesManuallyEdited_ = true; }
        }
    }
}
