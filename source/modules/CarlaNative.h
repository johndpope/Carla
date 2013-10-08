/*
 * Carla Native Plugin API
 * Copyright (C) 2012-2013 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#ifndef CARLA_NATIVE_H_INCLUDED
#define CARLA_NATIVE_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*!
 * @defgroup CarlaNativeAPI Carla Native API
 *
 * The Carla Native API
 * @{
 */

typedef void* HostHandle;
typedef void* PluginHandle;

// -----------------------------------------------------------------------
// enums

typedef enum {
    PLUGIN_CATEGORY_NONE      = 0, //!< Null plugin category.
    PLUGIN_CATEGORY_SYNTH     = 1, //!< A synthesizer or generator.
    PLUGIN_CATEGORY_DELAY     = 2, //!< A delay or reverberator.
    PLUGIN_CATEGORY_EQ        = 3, //!< An equalizer.
    PLUGIN_CATEGORY_FILTER    = 4, //!< A filter.
    PLUGIN_CATEGORY_DYNAMICS  = 5, //!< A 'dynamic' plugin (amplifier, compressor, gate, etc).
    PLUGIN_CATEGORY_MODULATOR = 6, //!< A 'modulator' plugin (chorus, flanger, phaser, etc).
    PLUGIN_CATEGORY_UTILITY   = 7, //!< An 'utility' plugin (analyzer, converter, mixer, etc).
    PLUGIN_CATEGORY_OTHER     = 8  //!< Misc plugin (used to check if the plugin has a category).
} PluginCategory;

typedef enum {
    PLUGIN_IS_RTSAFE           = 1 << 0,
    PLUGIN_IS_SYNTH            = 1 << 1,
    PLUGIN_HAS_GUI             = 1 << 2,
    PLUGIN_NEEDS_FIXED_BUFFERS = 1 << 3,
    PLUGIN_NEEDS_SINGLE_THREAD = 1 << 4,
    PLUGIN_NEEDS_UI_OPEN_SAVE  = 1 << 5,
    PLUGIN_USES_PANNING        = 1 << 6, // uses stereo balance if unset (default)
    PLUGIN_USES_STATE          = 1 << 7,
    PLUGIN_USES_TIME           = 1 << 8
} PluginHints;

typedef enum {
    PLUGIN_SUPPORTS_PROGRAM_CHANGES  = 1 << 0, // handles MIDI programs internally instead of host-exposed/exported
    PLUGIN_SUPPORTS_CONTROL_CHANGES  = 1 << 1,
    PLUGIN_SUPPORTS_CHANNEL_PRESSURE = 1 << 2,
    PLUGIN_SUPPORTS_NOTE_AFTERTOUCH  = 1 << 3,
    PLUGIN_SUPPORTS_PITCHBEND        = 1 << 4,
    PLUGIN_SUPPORTS_ALL_SOUND_OFF    = 1 << 5,
    PLUGIN_SUPPORTS_EVERYTHING       = (1 << 6)-1
} PluginSupports;

typedef enum {
    PARAMETER_IS_OUTPUT        = 1 << 0,
    PARAMETER_IS_ENABLED       = 1 << 1,
    PARAMETER_IS_AUTOMABLE     = 1 << 2,
    PARAMETER_IS_BOOLEAN       = 1 << 3,
    PARAMETER_IS_INTEGER       = 1 << 4,
    PARAMETER_IS_LOGARITHMIC   = 1 << 5,
    PARAMETER_USES_SAMPLE_RATE = 1 << 6,
    PARAMETER_USES_SCALEPOINTS = 1 << 7,
    PARAMETER_USES_CUSTOM_TEXT = 1 << 8
} ParameterHints;

typedef enum {
    PLUGIN_OPCODE_NULL                = 0, // nothing
    PLUGIN_OPCODE_BUFFER_SIZE_CHANGED = 1, // uses value
    PLUGIN_OPCODE_SAMPLE_RATE_CHANGED = 2, // uses opt
    PLUGIN_OPCODE_OFFLINE_CHANGED     = 3, // uses value (0=off, 1=on)
    PLUGIN_OPCODE_UI_NAME_CHANGED     = 4  // uses ptr
} PluginDispatcherOpcode;

typedef enum {
    HOST_OPCODE_NULL                  = 0,  // nothing
    HOST_OPCODE_SET_VOLUME            = 1,  // uses opt
    HOST_OPCODE_SET_DRYWET            = 2,  // uses opt
    HOST_OPCODE_SET_BALANCE_LEFT      = 3,  // uses opt
    HOST_OPCODE_SET_BALANCE_RIGHT     = 4,  // uses opt
    HOST_OPCODE_SET_PANNING           = 5,  // uses opt
    HOST_OPCODE_GET_PARAMETER_MIDI_CC = 6,  // uses index; return answer
    HOST_OPCODE_SET_PARAMETER_MIDI_CC = 7,  // uses index and value
    HOST_OPCODE_SET_PROCESS_PRECISION = 8,  // uses value
    HOST_OPCODE_UPDATE_PARAMETER      = 9,  // uses index, -1 for all
    HOST_OPCODE_UPDATE_MIDI_PROGRAM   = 10, // uses index, -1 for all; may use value for channel
    HOST_OPCODE_RELOAD_PARAMETERS     = 11, // nothing
    HOST_OPCODE_RELOAD_MIDI_PROGRAMS  = 12, // nothing
    HOST_OPCODE_RELOAD_ALL            = 13, // nothing
    HOST_OPCODE_UI_UNAVAILABLE        = 14  // nothing
} HostDispatcherOpcode;

// -----------------------------------------------------------------------
// base structs

typedef struct {
    const char* label;
    float value;
} ParameterScalePoint;

typedef struct {
    float def;
    float min;
    float max;
    float step;
    float stepSmall;
    float stepLarge;
} ParameterRanges;

#define PARAMETER_RANGES_DEFAULT_STEP       0.01f
#define PARAMETER_RANGES_DEFAULT_STEP_SMALL 0.0001f
#define PARAMETER_RANGES_DEFAULT_STEP_LARGE 0.1f

typedef struct {
    ParameterHints hints;
    const char* name;
    const char* unit;
    ParameterRanges ranges;

    uint32_t scalePointCount;
    ParameterScalePoint* scalePoints;
} Parameter;

typedef struct {
    uint8_t  port;
    uint32_t time;
    uint8_t  data[4];
    uint8_t  size;
} MidiEvent;

typedef struct {
    uint32_t bank;
    uint32_t program;
    const char* name;
} MidiProgram;

typedef struct {
    bool valid;

    int32_t bar;  //!< current bar
    int32_t beat; //!< current beat-within-bar
    int32_t tick; //!< current tick-within-beat
    double barStartTick;

    float beatsPerBar; //!< time signature "numerator"
    float beatType;    //!< time signature "denominator"

    double ticksPerBeat;
    double beatsPerMinute;
} TimeInfoBBT;

typedef struct {
    bool playing;
    uint64_t frame;
    uint64_t usecs;
    TimeInfoBBT bbt;
} TimeInfo;

// -----------------------------------------------------------------------
// HostDescriptor

typedef struct {
    HostHandle handle;
    const char* resourceDir;
    const char* uiName;

    uint32_t (*get_buffer_size)(HostHandle handle);
    double   (*get_sample_rate)(HostHandle handle);
    bool     (*is_offline)(HostHandle handle);

    const TimeInfo* (*get_time_info)(HostHandle handle);
    bool            (*write_midi_event)(HostHandle handle, const MidiEvent* event);

    void (*ui_parameter_changed)(HostHandle handle, uint32_t index, float value);
    void (*ui_midi_program_changed)(HostHandle handle, uint8_t channel, uint32_t bank, uint32_t program);
    void (*ui_custom_data_changed)(HostHandle handle, const char* key, const char* value);
    void (*ui_closed)(HostHandle handle);

    const char* (*ui_open_file)(HostHandle handle, bool isDir, const char* title, const char* filter);
    const char* (*ui_save_file)(HostHandle handle, bool isDir, const char* title, const char* filter);

    intptr_t (*dispatcher)(HostHandle handle, HostDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt);

} HostDescriptor;

// -----------------------------------------------------------------------
// PluginDescriptor

typedef struct _PluginDescriptor {
    const PluginCategory category;
    const PluginHints hints;
    const PluginSupports supports;
    const uint32_t audioIns;
    const uint32_t audioOuts;
    const uint32_t midiIns;
    const uint32_t midiOuts;
    const uint32_t paramIns;
    const uint32_t paramOuts;
    const char* const name;
    const char* const label;
    const char* const maker;
    const char* const copyright;

    PluginHandle (*instantiate)(const HostDescriptor* host);
    void         (*cleanup)(PluginHandle handle);

    uint32_t         (*get_parameter_count)(PluginHandle handle);
    const Parameter* (*get_parameter_info)(PluginHandle handle, uint32_t index);
    float            (*get_parameter_value)(PluginHandle handle, uint32_t index);
    const char*      (*get_parameter_text)(PluginHandle handle, uint32_t index, float value);

    uint32_t           (*get_midi_program_count)(PluginHandle handle);
    const MidiProgram* (*get_midi_program_info)(PluginHandle handle, uint32_t index);

    void (*set_parameter_value)(PluginHandle handle, uint32_t index, float value);
    void (*set_midi_program)(PluginHandle handle, uint8_t channel, uint32_t bank, uint32_t program);
    void (*set_custom_data)(PluginHandle handle, const char* key, const char* value);

    void (*ui_show)(PluginHandle handle, bool show);
    void (*ui_idle)(PluginHandle handle);

    void (*ui_set_parameter_value)(PluginHandle handle, uint32_t index, float value);
    void (*ui_set_midi_program)(PluginHandle handle, uint8_t channel, uint32_t bank, uint32_t program);
    void (*ui_set_custom_data)(PluginHandle handle, const char* key, const char* value);

    void (*activate)(PluginHandle handle);
    void (*deactivate)(PluginHandle handle);
    void (*process)(PluginHandle handle, float** inBuffer, float** outBuffer, uint32_t frames, const MidiEvent* midiEvents, uint32_t midiEventCount);

    char* (*get_state)(PluginHandle handle);
    void  (*set_state)(PluginHandle handle, const char* data);

    intptr_t (*dispatcher)(PluginHandle handle, PluginDispatcherOpcode opcode, int32_t index, intptr_t value, void* ptr, float opt);

} PluginDescriptor;

// -----------------------------------------------------------------------
// Register plugin

extern void carla_register_native_plugin(const PluginDescriptor* desc);

// -----------------------------------------------------------------------

/**@}*/

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CARLA_NATIVE_H_INCLUDED