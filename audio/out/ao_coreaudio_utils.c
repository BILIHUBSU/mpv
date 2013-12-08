/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file contains functions interacting with the CoreAudio framework
 * that are not specific to the AUHAL. These are split in a separate file for
 * the sake of readability. In the future the could be used by other AOs based
 * on CoreAudio but not the AUHAL (such as using AudioQueue services).
 */

#include "audio/format.h"
#include "osdep/timer.h"
#include "audio/out/ao_coreaudio_utils.h"
#include "audio/out/ao_coreaudio_properties.h"

char *fourcc_repr(void *talloc_ctx, uint32_t code)
{
    // Extract FourCC letters from the uint32_t and finde out if it's a valid
    // code that is made of letters.
    char fcc[4] = {
        (code >> 24) & 0xFF,
        (code >> 16) & 0xFF,
        (code >> 8)  & 0xFF,
        code         & 0xFF,
    };

    bool valid_fourcc = true;
    for (int i = 0; i < 4; i++)
        if (!isprint(fcc[i]))
            valid_fourcc = false;

    char *repr;
    if (valid_fourcc)
        repr = talloc_asprintf(talloc_ctx, "'%c%c%c%c'",
                               fcc[0], fcc[1], fcc[2], fcc[3]);
    else
        repr = talloc_asprintf(NULL, "%d", code);

    return repr;
}

bool check_ca_st(struct ao *ao, int level, OSStatus code, const char *message)
{
    if (code == noErr) return true;

    char *error_string = fourcc_repr(NULL, code);
    mp_msg_log(ao->log, level, "%s (%s)\n", message, error_string);
    talloc_free(error_string);

    return false;
}

char *ca_asbd_repr(const AudioStreamBasicDescription *asbd)
{
    char *result    = talloc_strdup(NULL, "");
    char *format    = fourcc_repr(result, asbd->mFormatID);
    uint32_t flags  = asbd->mFormatFlags;

    result = talloc_asprintf_append(result,
       "%7.1fHz %" PRIu32 "bit [%s]"
       "[%" PRIu32 "][%" PRIu32 "][%" PRIu32 "]"
       "[%" PRIu32 "][%" PRIu32 "] "
       "%s %s %s %s%s%s%s\n",
       asbd->mSampleRate, asbd->mBitsPerChannel, format,
       asbd->mFormatFlags, asbd->mBytesPerPacket, asbd->mFramesPerPacket,
       asbd->mBytesPerFrame, asbd->mChannelsPerFrame,
       (flags & kAudioFormatFlagIsFloat) ? "float" : "int",
       (flags & kAudioFormatFlagIsNonMixable) ? "" : "mixable",
       (flags & kAudioFormatFlagIsBigEndian) ? "BE" : "LE",
       (flags & kAudioFormatFlagIsSignedInteger) ? "S" : "U",
       (flags & kAudioFormatFlagIsPacked) ? " packed" : "",
       (flags & kAudioFormatFlagIsAlignedHigh) ? " aligned" : "",
       (flags & kAudioFormatFlagIsNonInterleaved) ? " P" : "");

    return result;
}

void ca_print_asbd(struct ao *ao, const char *description,
                   const AudioStreamBasicDescription *asbd)
{
    char *repr = ca_asbd_repr(asbd);
    MP_VERBOSE(ao, "%s %s", description, repr);
    talloc_free(repr);
}

static OSStatus print_formats(AudioStreamID stream, int stream_id,
                             char *format_kind_repr, int format_kind,
                             char **help)
{
    AudioStreamRangedDescription *formats;
    size_t n_formats;
    OSStatus err = CA_GET_ARY(stream, format_kind, &formats, &n_formats);

    if (err != noErr)
        return err;

    *help = talloc_asprintf_append(*help, "    %s:\n", format_kind_repr);
    for (int k = 0; k < n_formats; k++) {
        char *repr = ca_asbd_repr(&formats[k].mFormat);
        *help = talloc_asprintf_append(*help,
             "      - Stream %d, Format %d: %s", stream_id, k, repr);
        talloc_free(repr);
    }

    talloc_free(formats);
    return noErr;
}

void ca_print_device_list(struct ao *ao)
{
    char *help = talloc_strdup(NULL, "Available output devices:\n");

    AudioDeviceID *devs;
    size_t n_devs;

    OSStatus err =
        CA_GET_ARY(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                   &devs, &n_devs);
    talloc_steal(help, devs);

    CHECK_CA_ERROR("Failed to get list of output devices.");

    for (int i = 0; i < n_devs; i++) {
        char *name;
        OSStatus err = CA_GET_STR(devs[i], kAudioObjectPropertyName, &name);

        if (err == noErr)
            talloc_steal(devs, name);
        else
            name = "Unknown";

        help = talloc_asprintf_append(
                help, "  * %s (id: %" PRIu32 ")\n", name, devs[i]);


        AudioStreamID *streams = NULL;
        size_t n_streams;
        err = CA_GET_ARY_O(devs[i], kAudioDevicePropertyStreams,
                           &streams, &n_streams);
        talloc_steal(devs, streams);

        CHECK_CA_ERROR("could not get streams.");

        for (int j = 0; j < n_streams; j++) {
            err = print_formats(streams[j], j, "Physical Formats",
                                kAudioStreamPropertyAvailablePhysicalFormats,
                                &help);
            if (!CHECK_CA_WARN("could not get stream physical formats"))
                continue;

            err = print_formats(streams[j], j, "Virtual Formats",
                                kAudioStreamPropertyAvailableVirtualFormats,
                                &help);

            if (!CHECK_CA_WARN("could not get stream virtual formats"))
                continue;
        }
    }

coreaudio_error:
    MP_INFO(ao, "%s", help);
    talloc_free(help);
}

AudioStreamBasicDescription ca_make_asbd(int mp_format, int rate, int channels)
{
    AudioStreamBasicDescription asbd = (AudioStreamBasicDescription) {
        .mSampleRate       = rate,
        .mFormatID         = kAudioFormatLinearPCM,
        .mChannelsPerFrame = channels,
        .mBitsPerChannel   = af_fmt2bits(mp_format),
        .mFormatFlags      = kAudioFormatFlagIsPacked,
    };

    if ((mp_format & AF_FORMAT_POINT_MASK) == AF_FORMAT_F)
        asbd.mFormatFlags |= kAudioFormatFlagIsFloat;

    if ((mp_format & AF_FORMAT_SIGN_MASK) == AF_FORMAT_SI)
        asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;

    if ((mp_format & AF_FORMAT_END_MASK) == AF_FORMAT_BE)
        asbd.mFormatFlags |= kAudioFormatFlagIsBigEndian;

    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerPacket = asbd.mBytesPerFrame =
        asbd.mFramesPerPacket * asbd.mChannelsPerFrame *
        (asbd.mBitsPerChannel / 8);

    return asbd;
}

int ca_make_mp_format(AudioStreamBasicDescription asbd)
{

    int format = AF_FORMAT_UNKNOWN;
    const int bits  = asbd.mBitsPerChannel;
    const int flags = asbd.mFormatFlags;

    // convert bits
    format |= af_bits_to_mask(bits);

    // convert format
    if (flags & kAudioFormatFlagIsFloat) {
        format |= AF_FORMAT_F;
    } else {
        format |= AF_FORMAT_I;
        if (flags & kAudioFormatFlagIsSignedInteger)
            format |= AF_FORMAT_SI;
        else
            format |= AF_FORMAT_US;
    }

    // convert endianness
    if (flags & kAudioFormatFlagIsBigEndian)
        format |= AF_FORMAT_BE;
    else
        format |= AF_FORMAT_LE;

    return format;
}

static bool ca_match_fflags(int target, int matchee){
    int flags[4] = {
        kAudioFormatFlagIsFloat,
        kAudioFormatFlagIsSignedInteger,
        kAudioFormatFlagIsBigEndian,
        0
    };

    for (int i=0; flags[i]; i++)
        if ((target & flags[i]) != (matchee & flags[i]))
            return false;

    return true;
}

bool ca_asbd_matches(AudioStreamBasicDescription target,
                     AudioStreamBasicDescription matchee)
{
    return target.mFormatID == matchee.mFormatID;
}

bool ca_asbd_best(AudioStreamBasicDescription target,
                  AudioStreamBasicDescription matchee)
{
    return ca_asbd_matches(target, matchee) &&
        target.mBitsPerChannel   == matchee.mBitsPerChannel &&
        target.mSampleRate       == matchee.mSampleRate &&
        target.mChannelsPerFrame == matchee.mChannelsPerFrame &&
        ca_match_fflags(target.mFormatFlags, matchee.mFormatFlags);
}

int ca_asbd_better(AudioStreamBasicDescription target,
                   AudioStreamBasicDescription fst,
                   AudioStreamBasicDescription snd)
{
    // Check if one of the asbd's is unitialized. If so return the other.
    if (fst.mSampleRate == 0.0)
        return 1;

    if (snd.mSampleRate == 0.0)
        return -1;

    // Check if one of the asbd's has a matching channel count, while the
    // other does not.
    if (fst.mChannelsPerFrame == target.mChannelsPerFrame &&
        snd.mChannelsPerFrame != target.mChannelsPerFrame)
        return -1;

    if (fst.mChannelsPerFrame != target.mChannelsPerFrame &&
        snd.mChannelsPerFrame == target.mChannelsPerFrame)
        return 1;

    // At this point, channel count is the same: decide based on the sample
    // rate. Take the asbd that has the same or closest sample rate while still
    // being >= than the target sample rate (so that in the worst case we
    // upsample but never downsample).
    if (fst.mSampleRate < target.mSampleRate)
        return 1;

    if (snd.mSampleRate < target.mSampleRate)
        return -1;

    if (fst.mSampleRate > target.mSampleRate)
        return 1;
    else
        return -1;
}

OSStatus ca_property_listener(AudioObjectPropertySelector selector,
                              AudioObjectID object, uint32_t n_addresses,
                              const AudioObjectPropertyAddress addresses[],
                              void *data)
{
    void *talloc_ctx = talloc_new(NULL);

    for (int i = 0; i < n_addresses; i++) {
        if (addresses[i].mSelector == selector) {
            if (data) *(volatile int *)data = 1;
            break;
        }
    }
    talloc_free(talloc_ctx);
    return noErr;
}

OSStatus ca_stream_listener(AudioObjectID object, uint32_t n_addresses,
                            const AudioObjectPropertyAddress addresses[],
                            void *data)
{
    return ca_property_listener(kAudioStreamPropertyPhysicalFormat,
                                object, n_addresses, addresses, data);
}

static OSStatus ca_change_stream_listening(AudioObjectID device, int sel,
                                           void *flag, bool enabled)
{
    AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
        .mSelector = kAudioStreamPropertyVirtualFormat,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMaster,
    };

    if (enabled) {
        return AudioObjectAddPropertyListener(
            device, &p_addr, ca_stream_listener, flag);
    } else {
        return AudioObjectRemovePropertyListener(
            device, &p_addr, ca_stream_listener, flag);
    }
}

OSStatus ca_enable_stream_listener(AudioDeviceID device, int sel, void *flag) {
    return ca_change_stream_listening(device, sel, flag, true);
}

OSStatus ca_disable_stream_listener(AudioDeviceID device, int sel, void *flag) {
    return ca_change_stream_listening(device, sel, flag, false);
}

OSStatus ca_lock_device(AudioDeviceID device, pid_t *pid) {
    *pid = getpid();
    OSStatus err = CA_SET(device, kAudioDevicePropertyHogMode, pid);
    if (err != noErr)
        *pid = -1;

    return err;
}

OSStatus ca_unlock_device(AudioDeviceID device, pid_t *pid) {
    if (*pid == getpid()) {
        *pid = -1;
        return CA_SET(device, kAudioDevicePropertyHogMode, &pid);
    }
    return noErr;
}

static OSStatus ca_change_mixing(struct ao *ao, AudioDeviceID device,
                                 uint32_t val, bool *changed) {
    *changed = false;

    AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
        .mSelector = kAudioDevicePropertySupportsMixing,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMaster,
    };

    if (AudioObjectHasProperty(device, &p_addr)) {
        OSStatus err;
        Boolean writeable = 0;
        err = CA_SETTABLE(device, kAudioDevicePropertySupportsMixing,
                          &writeable);

        if (!CHECK_CA_WARN("can't tell if mixing property is settable")) {
            return err;
        }

        if (!writeable) {
            MP_INFO(ao, "mixing property is *not* settable");
            return noErr;
        }

        err = CA_SET(device, kAudioDevicePropertySupportsMixing, &val);
        if (err != noErr)
            return err;

        if (!CHECK_CA_WARN("can't set mix mode")) {
            return err;
        }

        *changed = true;
    }

    return noErr;
}

OSStatus ca_disable_mixing(struct ao *ao, AudioDeviceID device, bool *changed) {
    return ca_change_mixing(ao, device, 0, changed);
}

OSStatus ca_enable_mixing(struct ao *ao, AudioDeviceID device, bool changed) {
    if (changed) {
        bool dont_care = false;
        return ca_change_mixing(ao, device, 1, &dont_care);
    }

    return noErr;
}

bool ca_change_format_sync(struct ao *ao, AudioStreamID stream,
                           AudioStreamBasicDescription new_format, int sel)
{
    OSStatus err = CA_SET(stream, sel, &new_format);
    return CHECK_CA_WARN("error changing format");
}

bool ca_change_format(struct ao *ao, AudioStreamID stream,
                      AudioStreamBasicDescription new_format, int sel)
{
    OSStatus err = noErr;
    volatile int fmt_changed = 0;
    AudioStreamBasicDescription actual_format;

    err = CA_GET(stream, sel, &actual_format);
    if (!CHECK_CA_WARN("can't fetch format property")) {
        return false;
    }

    if (ca_asbd_best(actual_format, new_format)) {
        MP_ERR(ao, "requested format matches current format\n");
        return true;
    }

    err = ca_enable_stream_listener(stream, sel, (void *)&fmt_changed);
    if (!CHECK_CA_WARN("can't add format property listener")) {
        return false;
    }

    err = CA_SET(stream, sel, &new_format);
    if (!CHECK_CA_WARN("error changing format")) {
        return false;
    }

    // Setting the format is an asynchronous operation. We need to make sure
    // the change actually took place before reporting back the current
    // format to mpv's filter chain.
    for (int j = 0; !fmt_changed && j < 50; j++)
        mp_sleep_us(10000);

    if (!fmt_changed)
        MP_WARN(ao, "reached timeout while polling for format changes\n");

    err = ca_disable_stream_listener(stream, sel, (void *)&fmt_changed);
    if (!CHECK_CA_WARN("can't remove format property listener")) {
        return false;
    }

    return true;
}

static const int speaker_map[][2] = {
    { kAudioChannelLabel_Left,                 MP_SPEAKER_ID_FL   },
    { kAudioChannelLabel_Right,                MP_SPEAKER_ID_FR   },
    { kAudioChannelLabel_Center,               MP_SPEAKER_ID_FC   },
    { kAudioChannelLabel_LFEScreen,            MP_SPEAKER_ID_LFE  },
    { kAudioChannelLabel_LeftSurround,         MP_SPEAKER_ID_BL   },
    { kAudioChannelLabel_RightSurround,        MP_SPEAKER_ID_BR   },
    { kAudioChannelLabel_LeftCenter,           MP_SPEAKER_ID_FLC  },
    { kAudioChannelLabel_RightCenter,          MP_SPEAKER_ID_FRC  },
    { kAudioChannelLabel_CenterSurround,       MP_SPEAKER_ID_BC   },
    { kAudioChannelLabel_LeftSurroundDirect,   MP_SPEAKER_ID_SL   },
    { kAudioChannelLabel_RightSurroundDirect,  MP_SPEAKER_ID_SR   },
    { kAudioChannelLabel_TopCenterSurround,    MP_SPEAKER_ID_TC   },
    { kAudioChannelLabel_VerticalHeightLeft,   MP_SPEAKER_ID_TFL  },
    { kAudioChannelLabel_VerticalHeightCenter, MP_SPEAKER_ID_TFC  },
    { kAudioChannelLabel_VerticalHeightRight,  MP_SPEAKER_ID_TFR  },
    { kAudioChannelLabel_TopBackLeft,          MP_SPEAKER_ID_TBL  },
    { kAudioChannelLabel_TopBackCenter,        MP_SPEAKER_ID_TBC  },
    { kAudioChannelLabel_TopBackRight,         MP_SPEAKER_ID_TBR  },

    // unofficial extensions
    { kAudioChannelLabel_RearSurroundLeft,     MP_SPEAKER_ID_SDL  },
    { kAudioChannelLabel_RearSurroundRight,    MP_SPEAKER_ID_SDR  },
    { kAudioChannelLabel_LeftWide,             MP_SPEAKER_ID_WL   },
    { kAudioChannelLabel_RightWide,            MP_SPEAKER_ID_WR   },
    { kAudioChannelLabel_LFE2,                 MP_SPEAKER_ID_LFE2 },

    { kAudioChannelLabel_HeadphonesLeft,       MP_SPEAKER_ID_DL   },
    { kAudioChannelLabel_HeadphonesRight,      MP_SPEAKER_ID_DR   },

    { kAudioChannelLabel_Unknown,              -1 },
};

static int ca_label_to_mp_speaker_id(AudioChannelLabel label)
{
    for (int i = 0; speaker_map[i][0] != kAudioChannelLabel_Unknown; i++)
        if (speaker_map[i][0] == label)
            return speaker_map[i][1];
    return -1;
}

static bool ca_bitmap_from_ch_desc(struct ao *ao, AudioChannelLayout *layout,
                                   uint32_t *bitmap)
{
    // If the channel layout uses channel descriptions, from my
    // exepriments there are there three possibile cases:
    // * The description has a label kAudioChannelLabel_Unknown:
    //   Can't do anything about this (looks like non surround
    //   layouts are like this).
    // * The description uses positional information: this in
    //   theory could be used but one would have to map spatial
    //   positions to labels which is not really feasible.
    // * The description has a well known label which can be mapped
    //   to the waveextensible definition: this is the kind of
    //   descriptions we process here.
    size_t ch_num = layout->mNumberChannelDescriptions;
    bool all_channels_valid = true;

    for (int j=0; j < ch_num && all_channels_valid; j++) {
        AudioChannelLabel label = layout->mChannelDescriptions[j].mChannelLabel;
        const int mp_speaker_id = ca_label_to_mp_speaker_id(label);
        if (mp_speaker_id < 0) {
            MP_VERBOSE(ao, "channel label=%d unusable to build channel "
                           "bitmap, skipping layout\n", label);
            all_channels_valid = false;
        } else {
            *bitmap |= 1ULL << mp_speaker_id;
        }
    }

    return all_channels_valid;
}

static bool ca_bitmap_from_ch_tag(struct ao *ao, AudioChannelLayout *layout,
                                  uint32_t *bitmap)
{
    // This layout is defined exclusively by it's tag. Use the Audio
    // Format Services API to try and convert it to a bitmap that
    // mpv can use.
    uint32_t bitmap_size = sizeof(uint32_t);

    AudioChannelLayoutTag tag = layout->mChannelLayoutTag;
    OSStatus err = AudioFormatGetProperty(
        kAudioFormatProperty_BitmapForLayoutTag,
        sizeof(AudioChannelLayoutTag), &tag,
        &bitmap_size, bitmap);
    if (err != noErr) {
        MP_VERBOSE(ao, "channel layout tag=%d unusable to build channel "
                       "bitmap, skipping layout\n", tag);
        return false;
    } else {
        return true;
    }
}

void ca_bitmaps_from_layouts(struct ao *ao,
                             AudioChannelLayout *layouts, size_t n_layouts,
                             uint32_t **bitmaps, size_t *n_bitmaps)
{
    *n_bitmaps = 0;
    *bitmaps = talloc_array_size(NULL, sizeof(uint32_t), n_layouts);

    for (int i=0; i < n_layouts; i++) {
        uint32_t bitmap = 0;

        switch (layouts[i].mChannelLayoutTag) {
        case kAudioChannelLayoutTag_UseChannelBitmap:
            (*bitmaps)[(*n_bitmaps)++] = layouts[i].mChannelBitmap;
            break;

        case kAudioChannelLayoutTag_UseChannelDescriptions:
            if (ca_bitmap_from_ch_desc(ao, &layouts[i], &bitmap))
                (*bitmaps)[(*n_bitmaps)++] = bitmap;
            break;

        default:
            if (ca_bitmap_from_ch_tag(ao, &layouts[i], &bitmap))
                (*bitmaps)[(*n_bitmaps)++] = bitmap;
        }
    }
}
