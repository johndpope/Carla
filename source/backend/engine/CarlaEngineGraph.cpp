/*
 * Carla Plugin Host
 * Copyright (C) 2011-2014 Filipe Coelho <falktx@falktx.com>
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

#include "CarlaEngineGraph.hpp"
#include "CarlaEngineInternal.hpp"
#include "CarlaPlugin.hpp"

#include "CarlaMathUtils.hpp"
#include "CarlaMIDI.h"

using juce::AudioPluginInstance;
using juce::AudioProcessor;
using juce::AudioProcessorEditor;
using juce::FloatVectorOperations;
using juce::MemoryBlock;
using juce::PluginDescription;
using juce::String;
using juce::jmax;

CARLA_BACKEND_START_NAMESPACE

// -----------------------------------------------------------------------
// Rack Graph stuff

static inline
uint getCarlaRackPortIdFromName(const char* const shortname) noexcept
{
    if (std::strcmp(shortname, "AudioIn1") == 0 || std::strcmp(shortname, "audio-in1") == 0)
        return RACK_GRAPH_CARLA_PORT_AUDIO_IN1;
    if (std::strcmp(shortname, "AudioIn2") == 0 || std::strcmp(shortname, "audio-in2") == 0)
        return RACK_GRAPH_CARLA_PORT_AUDIO_IN2;
    if (std::strcmp(shortname, "AudioOut1") == 0 || std::strcmp(shortname, "audio-out1") == 0)
        return RACK_GRAPH_CARLA_PORT_AUDIO_OUT1;
    if (std::strcmp(shortname, "AudioOut2") == 0 || std::strcmp(shortname, "audio-out2") == 0)
        return RACK_GRAPH_CARLA_PORT_AUDIO_OUT2;
    if (std::strcmp(shortname, "MidiIn") == 0 || std::strcmp(shortname, "midi-in") == 0)
        return RACK_GRAPH_CARLA_PORT_MIDI_IN;
    if (std::strcmp(shortname, "MidiOut") == 0 || std::strcmp(shortname, "midi-out") == 0)
        return RACK_GRAPH_CARLA_PORT_MIDI_OUT;

    carla_stderr("CarlaBackend::getCarlaRackPortIdFromName(%s) - invalid short name", shortname);
    return RACK_GRAPH_CARLA_PORT_NULL;
}

static inline
const char* getCarlaRackFullPortNameFromId(const /*RackGraphCarlaPortIds*/ uint portId)
{
    switch (portId)
    {
    case RACK_GRAPH_CARLA_PORT_AUDIO_IN1:
        return "Carla:AudioIn1";
    case RACK_GRAPH_CARLA_PORT_AUDIO_IN2:
        return "Carla:AudioIn2";
    case RACK_GRAPH_CARLA_PORT_AUDIO_OUT1:
        return "Carla:AudioOut1";
    case RACK_GRAPH_CARLA_PORT_AUDIO_OUT2:
        return "Carla:AudioOut2";
    case RACK_GRAPH_CARLA_PORT_MIDI_IN:
        return "Carla:MidiIn";
    case RACK_GRAPH_CARLA_PORT_MIDI_OUT:
        return "Carla:MidiOut";
    //case RACK_GRAPH_CARLA_PORT_NULL:
    //case RACK_GRAPH_CARLA_PORT_MAX:
    //    break;
    }

    carla_stderr("CarlaBackend::getCarlaRackFullPortNameFromId(%i) - invalid port id", portId);
    return nullptr;
}

// -----------------------------------------------------------------------
// RackGraph Audio

RackGraph::Audio::Audio() noexcept
    : mutex(),
      connectedIn1(),
      connectedIn2(),
      connectedOut1(),
      connectedOut2()
#ifdef CARLA_PROPER_CPP11_SUPPORT
    , inBuf{nullptr, nullptr},
      inBufTmp{nullptr, nullptr},
      outBuf{nullptr, nullptr} {}
#else
    {
        inBuf[0]    = inBuf[1]    = nullptr;
        inBufTmp[0] = inBufTmp[1] = nullptr;
        outBuf[0]   = outBuf[1]   = nullptr;
    }
#endif

// -----------------------------------------------------------------------
// RackGraph MIDI

RackGraph::MIDI::MIDI() noexcept
    : ins(),
      outs() {}

const char* RackGraph::MIDI::getName(const bool isInput, const uint portId) const noexcept
{
    for (LinkedList<PortNameToId>::Itenerator it = isInput ? ins.begin() : outs.begin(); it.valid(); it.next())
    {
        static const PortNameToId portNameFallback = { 0, 0, { '\0' }, { '\0' } };

        const PortNameToId& portNameToId(it.getValue(portNameFallback));
        CARLA_SAFE_ASSERT_CONTINUE(portNameToId.group != 0);

        if (portNameToId.port == portId)
            return portNameToId.name;
    }

    return nullptr;
}

uint RackGraph::MIDI::getPortId(const bool isInput, const char portName[], bool* const ok) const noexcept
{
    for (LinkedList<PortNameToId>::Itenerator it = isInput ? ins.begin() : outs.begin(); it.valid(); it.next())
    {
        static const PortNameToId portNameFallback = { 0, 0, { '\0' }, { '\0' } };

        const PortNameToId& portNameToId(it.getValue(portNameFallback));
        CARLA_SAFE_ASSERT_CONTINUE(portNameToId.group != 0);

        if (std::strncmp(portNameToId.name, portName, STR_MAX) == 0)
        {
            if (ok != nullptr)
                *ok = true;
            return portNameToId.port;
        }
    }

    if (ok != nullptr)
        *ok = false;
    return 0;
}

// -----------------------------------------------------------------------
// RackGraph

RackGraph::RackGraph(const uint32_t bufferSize, const uint32_t ins, const uint32_t outs) noexcept
    : connections(),
      inputs(ins),
      outputs(outs),
      isOffline(false),
      retCon(),
      audio(),
      midi()
{
    setBufferSize(bufferSize);
}

RackGraph::~RackGraph() noexcept
{
    clearConnections();
}

void RackGraph::setBufferSize(const uint32_t bufferSize) noexcept
{
    const int bufferSizei(static_cast<int>(bufferSize));

    if (audio.inBuf[0]    != nullptr) { delete[] audio.inBuf[0];    audio.inBuf[0]    = nullptr; }
    if (audio.inBuf[1]    != nullptr) { delete[] audio.inBuf[1];    audio.inBuf[1]    = nullptr; }
    if (audio.inBufTmp[0] != nullptr) { delete[] audio.inBufTmp[0]; audio.inBufTmp[0] = nullptr; }
    if (audio.inBufTmp[1] != nullptr) { delete[] audio.inBufTmp[1]; audio.inBufTmp[1] = nullptr; }
    if (audio.outBuf[0]   != nullptr) { delete[] audio.outBuf[0];   audio.outBuf[0]   = nullptr; }
    if (audio.outBuf[1]   != nullptr) { delete[] audio.outBuf[1];   audio.outBuf[1]   = nullptr; }

    CARLA_SAFE_ASSERT_RETURN(bufferSize > 0,);

    try {
        audio.inBufTmp[0] = new float[bufferSize];
        audio.inBufTmp[1] = new float[bufferSize];

        if (inputs > 0 || outputs > 0)
        {
            audio.inBuf[0]  = new float[bufferSize];
            audio.inBuf[1]  = new float[bufferSize];
            audio.outBuf[0] = new float[bufferSize];
            audio.outBuf[1] = new float[bufferSize];
        }
    }
    catch(...) {
        if (audio.inBufTmp[0] != nullptr) { delete[] audio.inBufTmp[0]; audio.inBufTmp[0] = nullptr; }
        if (audio.inBufTmp[1] != nullptr) { delete[] audio.inBufTmp[1]; audio.inBufTmp[1] = nullptr; }

        if (inputs > 0 || outputs > 0)
        {
            if (audio.inBuf[0]  != nullptr) { delete[] audio.inBuf[0];  audio.inBuf[0]  = nullptr; }
            if (audio.inBuf[1]  != nullptr) { delete[] audio.inBuf[1];  audio.inBuf[1]  = nullptr; }
            if (audio.outBuf[0] != nullptr) { delete[] audio.outBuf[0]; audio.outBuf[0] = nullptr; }
            if (audio.outBuf[1] != nullptr) { delete[] audio.outBuf[1]; audio.outBuf[1] = nullptr; }
        }
        return;
    }

    FloatVectorOperations::clear(audio.inBufTmp[0], bufferSizei);
    FloatVectorOperations::clear(audio.inBufTmp[1], bufferSizei);

    if (inputs > 0 || outputs > 0)
    {
        FloatVectorOperations::clear(audio.inBuf[0],  bufferSizei);
        FloatVectorOperations::clear(audio.inBuf[1],  bufferSizei);
        FloatVectorOperations::clear(audio.outBuf[0], bufferSizei);
        FloatVectorOperations::clear(audio.outBuf[1], bufferSizei);
    }
}

void RackGraph::setOffline(const bool offline) noexcept
{
    isOffline = offline;
}

bool RackGraph::connect(CarlaEngine* const engine, const uint groupA, const uint portA, const uint groupB, const uint portB) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr, false);

    uint otherGroup, otherPort, carlaPort;

    if (groupA == RACK_GRAPH_GROUP_CARLA)
    {
        CARLA_SAFE_ASSERT_RETURN(groupB != RACK_GRAPH_GROUP_CARLA, false);

        carlaPort  = portA;
        otherGroup = groupB;
        otherPort  = portB;
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(groupB == RACK_GRAPH_GROUP_CARLA, false);

        carlaPort  = portB;
        otherGroup = groupA;
        otherPort  = portA;
    }

    CARLA_SAFE_ASSERT_RETURN(carlaPort > RACK_GRAPH_CARLA_PORT_NULL && carlaPort < RACK_GRAPH_CARLA_PORT_MAX, false);
    CARLA_SAFE_ASSERT_RETURN(otherGroup > RACK_GRAPH_GROUP_CARLA && otherGroup < RACK_GRAPH_GROUP_MAX, false);

    bool makeConnection = false;

    switch (carlaPort)
    {
    case RACK_GRAPH_CARLA_PORT_AUDIO_IN1:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_AUDIO_IN, false);
        audio.mutex.lock();
        makeConnection = audio.connectedIn1.append(otherPort);
        audio.mutex.unlock();
        break;

    case RACK_GRAPH_CARLA_PORT_AUDIO_IN2:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_AUDIO_IN, false);
        audio.mutex.lock();
        makeConnection = audio.connectedIn2.append(otherPort);
        audio.mutex.unlock();
        break;

    case RACK_GRAPH_CARLA_PORT_AUDIO_OUT1:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_AUDIO_OUT, false);
        audio.mutex.lock();
        makeConnection = audio.connectedOut1.append(otherPort);
        audio.mutex.unlock();
        break;

    case RACK_GRAPH_CARLA_PORT_AUDIO_OUT2:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_AUDIO_OUT, false);
        audio.mutex.lock();
        makeConnection = audio.connectedOut2.append(otherPort);
        audio.mutex.unlock();
        break;

    case RACK_GRAPH_CARLA_PORT_MIDI_IN:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_MIDI_IN, false);
        if (const char* const portName = midi.getName(true, otherPort))
            makeConnection = engine->connectRackMidiInPort(portName);
        break;

    case RACK_GRAPH_CARLA_PORT_MIDI_OUT:
        CARLA_SAFE_ASSERT_RETURN(otherGroup == RACK_GRAPH_GROUP_MIDI_OUT, false);
        if (const char* const portName = midi.getName(false, otherPort))
            makeConnection = engine->connectRackMidiOutPort(portName);
        break;
    }

    if (! makeConnection)
    {
        engine->setLastError("Invalid rack connection");
        return false;
    }

    ConnectionToId connectionToId;
    connectionToId.setData(++connections.lastId, groupA, portA, groupB, portB);

    char strBuf[STR_MAX+1];
    strBuf[STR_MAX] = '\0';
    std::snprintf(strBuf, STR_MAX, "%u:%u:%u:%u", groupA, portA, groupB, portB);

    engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, connectionToId.id, 0, 0, 0.0f, strBuf);

    connections.list.append(connectionToId);
    return true;
}

bool RackGraph::disconnect(CarlaEngine* const engine, const uint connectionId) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr, false);
    CARLA_SAFE_ASSERT_RETURN(connections.list.count() > 0, false);

    for (LinkedList<ConnectionToId>::Itenerator it=connections.list.begin(); it.valid(); it.next())
    {
        static const ConnectionToId fallback = { 0, 0, 0, 0, 0 };

        const ConnectionToId& connectionToId(it.getValue(fallback));
        CARLA_SAFE_ASSERT_CONTINUE(connectionToId.id != 0);

        if (connectionToId.id != connectionId)
            continue;

        uint otherGroup, otherPort, carlaPort;

        if (connectionToId.groupA == RACK_GRAPH_GROUP_CARLA)
        {
            CARLA_SAFE_ASSERT_RETURN(connectionToId.groupB != RACK_GRAPH_GROUP_CARLA, false);

            carlaPort  = connectionToId.portA;
            otherGroup = connectionToId.groupB;
            otherPort  = connectionToId.portB;
        }
        else
        {
            CARLA_SAFE_ASSERT_RETURN(connectionToId.groupB == RACK_GRAPH_GROUP_CARLA, false);

            carlaPort  = connectionToId.portB;
            otherGroup = connectionToId.groupA;
            otherPort  = connectionToId.portA;
        }

        CARLA_SAFE_ASSERT_RETURN(carlaPort > RACK_GRAPH_CARLA_PORT_NULL && carlaPort < RACK_GRAPH_CARLA_PORT_MAX, false);
        CARLA_SAFE_ASSERT_RETURN(otherGroup > RACK_GRAPH_GROUP_CARLA && otherGroup < RACK_GRAPH_GROUP_MAX, false);

        bool makeDisconnection = false;

        switch (carlaPort)
        {
        case RACK_GRAPH_CARLA_PORT_AUDIO_IN1:
            audio.mutex.lock();
            makeDisconnection = audio.connectedIn1.removeOne(otherPort);
            audio.mutex.unlock();
            break;

        case RACK_GRAPH_CARLA_PORT_AUDIO_IN2:
            audio.mutex.lock();
            makeDisconnection = audio.connectedIn2.removeOne(otherPort);
            audio.mutex.unlock();
            break;

        case RACK_GRAPH_CARLA_PORT_AUDIO_OUT1:
            audio.mutex.lock();
            makeDisconnection = audio.connectedOut1.removeOne(otherPort);
            audio.mutex.unlock();
            break;

        case RACK_GRAPH_CARLA_PORT_AUDIO_OUT2:
            audio.mutex.lock();
            makeDisconnection = audio.connectedOut2.removeOne(otherPort);
            audio.mutex.unlock();
            break;

        case RACK_GRAPH_CARLA_PORT_MIDI_IN:
            if (const char* const portName = midi.getName(true, otherPort))
                makeDisconnection = engine->disconnectRackMidiInPort(portName);
            break;

        case RACK_GRAPH_CARLA_PORT_MIDI_OUT:
            if (const char* const portName = midi.getName(false, otherPort))
                makeDisconnection = engine->disconnectRackMidiOutPort(portName);
            break;
        }

        if (! makeDisconnection)
        {
            engine->setLastError("Invalid rack connection");
            return false;
        }

        engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED, connectionToId.id, 0, 0, 0.0f, nullptr);

        connections.list.remove(it);
        return true;
    }

    engine->setLastError("Failed to find connection");
    return false;
}

void RackGraph::clearConnections() noexcept
{
    connections.clear();

    audio.mutex.lock();
    audio.connectedIn1.clear();
    audio.connectedIn2.clear();
    audio.connectedOut1.clear();
    audio.connectedOut2.clear();
    audio.mutex.unlock();

    midi.ins.clear();
    midi.outs.clear();
}

const char* const* RackGraph::getConnections() const noexcept
{
    if (connections.list.count() == 0)
        return nullptr;

    CarlaStringList connList;

    char strBuf[STR_MAX+1];
    strBuf[STR_MAX] = '\0';

    for (LinkedList<ConnectionToId>::Itenerator it=connections.list.begin(); it.valid(); it.next())
    {
        static const ConnectionToId fallback = { 0, 0, 0, 0, 0 };

        const ConnectionToId& connectionToId(it.getValue(fallback));
        CARLA_SAFE_ASSERT_CONTINUE(connectionToId.id != 0);

        uint otherGroup, otherPort, carlaPort;

        if (connectionToId.groupA == RACK_GRAPH_GROUP_CARLA)
        {
            CARLA_SAFE_ASSERT_CONTINUE(connectionToId.groupB != RACK_GRAPH_GROUP_CARLA);

            carlaPort  = connectionToId.portA;
            otherGroup = connectionToId.groupB;
            otherPort  = connectionToId.portB;
        }
        else
        {
            CARLA_SAFE_ASSERT_CONTINUE(connectionToId.groupB == RACK_GRAPH_GROUP_CARLA);

            carlaPort  = connectionToId.portB;
            otherGroup = connectionToId.groupA;
            otherPort  = connectionToId.portA;
        }

        CARLA_SAFE_ASSERT_CONTINUE(carlaPort > RACK_GRAPH_CARLA_PORT_NULL && carlaPort < RACK_GRAPH_CARLA_PORT_MAX);
        CARLA_SAFE_ASSERT_CONTINUE(otherGroup > RACK_GRAPH_GROUP_CARLA && otherGroup < RACK_GRAPH_GROUP_MAX);

        switch (carlaPort)
        {
        case RACK_GRAPH_CARLA_PORT_AUDIO_IN1:
        case RACK_GRAPH_CARLA_PORT_AUDIO_IN2:
            std::snprintf(strBuf, STR_MAX, "AudioIn:%i", otherPort);
            connList.append(strBuf);
            connList.append(getCarlaRackFullPortNameFromId(carlaPort));
            break;

        case RACK_GRAPH_CARLA_PORT_AUDIO_OUT1:
        case RACK_GRAPH_CARLA_PORT_AUDIO_OUT2:
            std::snprintf(strBuf, STR_MAX, "AudioOut:%i", otherPort);
            connList.append(getCarlaRackFullPortNameFromId(carlaPort));
            connList.append(strBuf);
            break;

        case RACK_GRAPH_CARLA_PORT_MIDI_IN:
            std::snprintf(strBuf, STR_MAX, "MidiIn:%s", midi.getName(true, otherPort));
            connList.append(strBuf);
            connList.append(getCarlaRackFullPortNameFromId(carlaPort));
            break;

        case RACK_GRAPH_CARLA_PORT_MIDI_OUT:
            std::snprintf(strBuf, STR_MAX, "MidiOut:%s", midi.getName(false, otherPort));
            connList.append(getCarlaRackFullPortNameFromId(carlaPort));
            connList.append(strBuf);
            break;
        }
    }

    if (connList.count() == 0)
        return nullptr;

    retCon = connList.toCharStringListPtr();

    return retCon;
}

bool RackGraph::getGroupAndPortIdFromFullName(const char* const fullPortName, uint& groupId, uint& portId) const noexcept
{
    CARLA_SAFE_ASSERT_RETURN(fullPortName != nullptr && fullPortName[0] != '\0', false);

    if (std::strncmp(fullPortName, "Carla:", 6) == 0)
    {
        groupId = RACK_GRAPH_GROUP_CARLA;
        portId  = getCarlaRackPortIdFromName(fullPortName+6);

        if (portId > RACK_GRAPH_CARLA_PORT_NULL && portId < RACK_GRAPH_CARLA_PORT_MAX)
            return true;
    }
    else if (std::strncmp(fullPortName, "AudioIn:", 8) == 0)
    {
        groupId = RACK_GRAPH_GROUP_AUDIO_IN;

        if (const int portTest = std::atoi(fullPortName+8))
        {
            portId = static_cast<uint>(portTest);
            return true;
        }
    }
    else if (std::strncmp(fullPortName, "AudioOut:", 9) == 0)
    {
        groupId = RACK_GRAPH_GROUP_AUDIO_OUT;

        if (const int portTest = std::atoi(fullPortName+9))
        {
            portId = static_cast<uint>(portTest);
            return true;
        }
    }
    else if (std::strncmp(fullPortName, "MidiIn:", 7) == 0)
    {
        groupId = RACK_GRAPH_GROUP_MIDI_IN;

        if (const char* const portName = fullPortName+7)
        {
            bool ok;
            portId = midi.getPortId(true, portName, &ok);
            return ok;
        }
    }
    else if (std::strncmp(fullPortName, "MidiOut:", 8) == 0)
    {
        groupId = RACK_GRAPH_GROUP_MIDI_OUT;

        if (const char* const portName = fullPortName+8)
        {
            bool ok;
            portId = midi.getPortId(false, portName, &ok);
            return ok;
        }
    }

    return false;
}

void RackGraph::process(CarlaEngine::ProtectedData* const data, const float* inBufReal[2], float* outBuf[2], const uint32_t frames)
{
    CARLA_SAFE_ASSERT_RETURN(data != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(data->events.in != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(data->events.out != nullptr,);

    const int iframes(static_cast<int>(frames));

    // safe copy
    float inBuf0[frames];
    float inBuf1[frames];
    const float* inBuf[2] = { inBuf0, inBuf1 };

    // initialize audio inputs
    FloatVectorOperations::copy(inBuf0, inBufReal[0], iframes);
    FloatVectorOperations::copy(inBuf1, inBufReal[1], iframes);

    // initialize audio outputs (zero)
    FloatVectorOperations::clear(outBuf[0], iframes);
    FloatVectorOperations::clear(outBuf[1], iframes);

    // initialize event outputs (zero)
    carla_zeroStruct<EngineEvent>(data->events.out, kMaxEngineEventInternalCount);

    bool processed = false;

    uint32_t oldAudioInCount = 0;
    uint32_t oldMidiOutCount = 0;

    // process plugins
    for (uint i=0; i < data->curPluginCount; ++i)
    {
        CarlaPlugin* const plugin = data->plugins[i].plugin;

        if (plugin == nullptr || ! plugin->isEnabled() || ! plugin->tryLock(isOffline))
            continue;

        if (processed)
        {
            // initialize audio inputs (from previous outputs)
            FloatVectorOperations::copy(inBuf0, outBuf[0], iframes);
            FloatVectorOperations::copy(inBuf1, outBuf[1], iframes);

            // initialize audio outputs (zero)
            FloatVectorOperations::clear(outBuf[0], iframes);
            FloatVectorOperations::clear(outBuf[1], iframes);

            // if plugin has no midi out, add previous events
            if (oldMidiOutCount == 0 && data->events.in[0].type != kEngineEventTypeNull)
            {
                if (data->events.out[0].type != kEngineEventTypeNull)
                {
                    // TODO: carefully add to input, sorted events
                }
                // else nothing needed
            }
            else
            {
                // initialize event inputs from previous outputs
                carla_copyStruct<EngineEvent>(data->events.in, data->events.out, kMaxEngineEventInternalCount);

                // initialize event outputs (zero)
                carla_zeroStruct<EngineEvent>(data->events.out, kMaxEngineEventInternalCount);
            }
        }

        oldAudioInCount = plugin->getAudioInCount();
        oldMidiOutCount = plugin->getMidiOutCount();

        // process
        plugin->initBuffers();
        plugin->process(inBuf, outBuf, nullptr, nullptr, frames);
        plugin->unlock();

        // if plugin has no audio inputs, add input buffer
        if (oldAudioInCount == 0)
        {
            FloatVectorOperations::add(outBuf[0], inBuf0, iframes);
            FloatVectorOperations::add(outBuf[1], inBuf1, iframes);
        }

        // set peaks
        {
            EnginePluginData& pluginData(data->plugins[i]);

            juce::Range<float> range;

            if (oldAudioInCount > 0)
            {
                range = FloatVectorOperations::findMinAndMax(inBuf0, iframes);
                pluginData.insPeak[0] = carla_maxLimited<float>(std::abs(range.getStart()), std::abs(range.getEnd()), 1.0f);

                range = FloatVectorOperations::findMinAndMax(inBuf1, iframes);
                pluginData.insPeak[1] = carla_maxLimited<float>(std::abs(range.getStart()), std::abs(range.getEnd()), 1.0f);
            }
            else
            {
                pluginData.insPeak[0] = 0.0f;
                pluginData.insPeak[1] = 0.0f;
            }

            if (plugin->getAudioOutCount() > 0)
            {
                range = FloatVectorOperations::findMinAndMax(outBuf[0], iframes);
                pluginData.outsPeak[0] = carla_maxLimited<float>(std::abs(range.getStart()), std::abs(range.getEnd()), 1.0f);

                range = FloatVectorOperations::findMinAndMax(outBuf[1], iframes);
                pluginData.outsPeak[1] = carla_maxLimited<float>(std::abs(range.getStart()), std::abs(range.getEnd()), 1.0f);
            }
            else
            {
                pluginData.outsPeak[0] = 0.0f;
                pluginData.outsPeak[1] = 0.0f;
            }
        }

        processed = true;
    }
}

void RackGraph::processHelper(CarlaEngine::ProtectedData* const data, const float* const* const inBuf, float* const* const outBuf, const uint32_t frames)
{
    CARLA_SAFE_ASSERT_RETURN(audio.outBuf[1] != nullptr,);

    const int iframes(static_cast<int>(frames));

    const CarlaRecursiveMutexLocker _cml(audio.mutex);

    if (inBuf != nullptr && inputs > 0)
    {
        bool noConnections = true;

        // connect input buffers
        for (LinkedList<uint>::Itenerator it = audio.connectedIn1.begin(); it.valid(); it.next())
        {
            const uint& port(it.getValue(0));
            CARLA_SAFE_ASSERT_CONTINUE(port != 0);
            CARLA_SAFE_ASSERT_CONTINUE(port < inputs);

            if (noConnections)
            {
                FloatVectorOperations::copy(audio.inBuf[0], inBuf[port], iframes);
                noConnections = false;
            }
            else
            {
                FloatVectorOperations::add(audio.inBuf[0], inBuf[port], iframes);
            }
        }

        if (noConnections)
            FloatVectorOperations::clear(audio.inBuf[0], iframes);

        noConnections = true;

        for (LinkedList<uint>::Itenerator it = audio.connectedIn2.begin(); it.valid(); it.next())
        {
            const uint& port(it.getValue(0));
            CARLA_SAFE_ASSERT_CONTINUE(port != 0);
            CARLA_SAFE_ASSERT_CONTINUE(port < inputs);

            if (noConnections)
            {
                FloatVectorOperations::copy(audio.inBuf[1], inBuf[port], iframes);
                noConnections = false;
            }
            else
            {
                FloatVectorOperations::add(audio.inBuf[1], inBuf[port], iframes);
            }
        }

        if (noConnections)
            FloatVectorOperations::clear(audio.inBuf[1], iframes);
    }
    else
    {
        FloatVectorOperations::clear(audio.inBuf[0], iframes);
        FloatVectorOperations::clear(audio.inBuf[1], iframes);
    }

    FloatVectorOperations::clear(audio.outBuf[0], iframes);
    FloatVectorOperations::clear(audio.outBuf[1], iframes);

    // process
    process(data, const_cast<const float**>(audio.inBuf), audio.outBuf, frames);

    // connect output buffers
    if (audio.connectedOut1.count() != 0)
    {
        for (LinkedList<uint>::Itenerator it = audio.connectedOut1.begin(); it.valid(); it.next())
        {
            const uint& port(it.getValue(0));
            CARLA_SAFE_ASSERT_CONTINUE(port > 0);
            CARLA_SAFE_ASSERT_CONTINUE(port <= outputs);

            FloatVectorOperations::add(outBuf[port-1], audio.outBuf[0], iframes);
        }
    }

    if (audio.connectedOut2.count() != 0)
    {
        for (LinkedList<uint>::Itenerator it = audio.connectedOut2.begin(); it.valid(); it.next())
        {
            const uint& port(it.getValue(0));
            CARLA_SAFE_ASSERT_CONTINUE(port > 0);
            CARLA_SAFE_ASSERT_CONTINUE(port <= outputs);

            FloatVectorOperations::add(outBuf[port-1], audio.outBuf[1], iframes);
        }
    }
}

// -----------------------------------------------------------------------
// Patchbay Graph stuff

static const uint32_t kAudioInputPortOffset  = MAX_PATCHBAY_PLUGINS*1;
static const uint32_t kAudioOutputPortOffset = MAX_PATCHBAY_PLUGINS*2;
static const uint32_t kMidiInputPortOffset   = MAX_PATCHBAY_PLUGINS*3;
static const uint32_t kMidiOutputPortOffset  = MAX_PATCHBAY_PLUGINS*3+1;

static const uint kMidiChannelIndex = static_cast<uint>(AudioProcessorGraph::midiChannelIndex);

static inline
bool adjustPatchbayPortIdForJuce(uint& portId)
{
    CARLA_SAFE_ASSERT_RETURN(portId >= kAudioInputPortOffset, false);
    CARLA_SAFE_ASSERT_RETURN(portId <= kMidiOutputPortOffset, false);

    if (portId == kMidiInputPortOffset)
    {
        portId = kMidiChannelIndex;
        return true;
    }
    if (portId == kMidiOutputPortOffset)
    {
        portId = kMidiChannelIndex;
        return true;
    }
    if (portId >= kAudioOutputPortOffset)
    {
        portId -= kAudioOutputPortOffset;
        return true;
    }
    if (portId >= kAudioInputPortOffset)
    {
        portId -= kAudioInputPortOffset;
        return true;
    }

    return false;
}

static inline
const String getProcessorFullPortName(AudioProcessor* const proc, const uint32_t portId)
{
    CARLA_SAFE_ASSERT_RETURN(proc != nullptr, String());
    CARLA_SAFE_ASSERT_RETURN(portId >= kAudioInputPortOffset, String());
    CARLA_SAFE_ASSERT_RETURN(portId <= kMidiOutputPortOffset, String());

    String fullPortName(proc->getName());

    if (portId == kMidiOutputPortOffset)
    {
        fullPortName += ":events-out";
    }
    else if (portId == kMidiInputPortOffset)
    {
        fullPortName += ":events-in";
    }
    else if (portId >= kAudioOutputPortOffset)
    {
        CARLA_SAFE_ASSERT_RETURN(proc->getNumOutputChannels() > 0, String());
        fullPortName += ":" + proc->getOutputChannelName(static_cast<int>(portId-kAudioOutputPortOffset));
    }
    else if (portId >= kAudioInputPortOffset)
    {
        CARLA_SAFE_ASSERT_RETURN(proc->getNumInputChannels() > 0, String());
        fullPortName += ":" + proc->getInputChannelName(static_cast<int>(portId-kAudioInputPortOffset));
    }
    else
    {
        return String();
    }

    return fullPortName;
}

static inline
void addNodeToPatchbay(CarlaEngine* const engine, const uint32_t groupId, const int clientId, const AudioProcessor* const proc)
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(proc != nullptr,);

    const int icon((clientId >= 0) ? PATCHBAY_ICON_PLUGIN : PATCHBAY_ICON_HARDWARE);
    engine->callback(ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, groupId, icon, clientId, 0.0f, proc->getName().toRawUTF8());

    for (int i=0, numInputs=proc->getNumInputChannels(); i<numInputs; ++i)
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, groupId, static_cast<int>(kAudioInputPortOffset)+i,
                         PATCHBAY_PORT_TYPE_AUDIO|PATCHBAY_PORT_IS_INPUT, 0.0f, proc->getInputChannelName(i).toRawUTF8());
    }

    for (int i=0, numOutputs=proc->getNumOutputChannels(); i<numOutputs; ++i)
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, groupId, static_cast<int>(kAudioOutputPortOffset)+i,
                         PATCHBAY_PORT_TYPE_AUDIO, 0.0f, proc->getOutputChannelName(i).toRawUTF8());
    }

    if (proc->acceptsMidi())
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, groupId, static_cast<int>(kMidiInputPortOffset),
                         PATCHBAY_PORT_TYPE_MIDI|PATCHBAY_PORT_IS_INPUT, 0.0f, "events-in");
    }

    if (proc->producesMidi())
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, groupId, static_cast<int>(kMidiOutputPortOffset),
                         PATCHBAY_PORT_TYPE_MIDI, 0.0f, "events-out");
    }
}

static inline
void removeNodeFromPatchbay(CarlaEngine* const engine, const uint32_t groupId, const AudioProcessor* const proc)
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(proc != nullptr,);

    for (int i=0, numInputs=proc->getNumInputChannels(); i<numInputs; ++i)
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_REMOVED, groupId, static_cast<int>(kAudioInputPortOffset)+i,
                         0, 0.0f, nullptr);
    }

    for (int i=0, numOutputs=proc->getNumOutputChannels(); i<numOutputs; ++i)
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_REMOVED, groupId, static_cast<int>(kAudioOutputPortOffset)+i,
                         0, 0.0f, nullptr);
    }

    if (proc->acceptsMidi())
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_REMOVED, groupId, static_cast<int>(kMidiInputPortOffset),
                         0, 0.0f, nullptr);
    }

    if (proc->producesMidi())
    {
        engine->callback(ENGINE_CALLBACK_PATCHBAY_PORT_REMOVED, groupId, static_cast<int>(kMidiOutputPortOffset),
                         0, 0.0f, nullptr);
    }

    engine->callback(ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED, groupId, 0, 0, 0.0f, nullptr);
}

// -----------------------------------------------------------------------

class CarlaPluginInstance : public AudioPluginInstance
{
public:
    CarlaPluginInstance(CarlaPlugin* const plugin)
        : fPlugin(plugin),
          leakDetector_CarlaPluginInstance()
    {
        setPlayConfigDetails(static_cast<int>(fPlugin->getAudioInCount()),
                             static_cast<int>(fPlugin->getAudioOutCount()),
                             getSampleRate(), getBlockSize());
    }

    ~CarlaPluginInstance() override
    {
    }

    // -------------------------------------------------------------------

    void* getPlatformSpecificData() noexcept override
    {
        return fPlugin;
    }

    void fillInPluginDescription(PluginDescription& d) const override
    {
        d.pluginFormatName = "Carla";
        d.category = "Carla Plugin";
        d.version = "1.0";

        CARLA_SAFE_ASSERT_RETURN(fPlugin != nullptr,);

        char strBuf[STR_MAX+1];
        strBuf[STR_MAX] = '\0';

        fPlugin->getRealName(strBuf);
        d.name = strBuf;

        fPlugin->getLabel(strBuf);
        d.descriptiveName = strBuf;

        fPlugin->getMaker(strBuf);
        d.manufacturerName = strBuf;

        d.uid = d.name.hashCode();
        d.isInstrument = (fPlugin->getHints() & PLUGIN_IS_SYNTH);

        d.numInputChannels  = static_cast<int>(fPlugin->getAudioInCount());
        d.numOutputChannels = static_cast<int>(fPlugin->getAudioOutCount());
        //d.hasSharedContainer = true;
    }

    // -------------------------------------------------------------------

    const String getName() const override
    {
        return fPlugin->getName();
    }

    void processBlock(AudioSampleBuffer& audio, MidiBuffer& midi)
    {
        if (! fPlugin->isEnabled())
        {
            audio.clear();
            midi.clear();
            return;
        }

        CarlaEngine* const engine(fPlugin->getEngine());
        CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);

        if (! fPlugin->tryLock(engine->isOffline()))
        {
            audio.clear();
            midi.clear();
            return;
        }

        fPlugin->initBuffers();

        if (CarlaEngineEventPort* const port = fPlugin->getDefaultEventInPort())
        {
            EngineEvent* const engineEvents(port->fBuffer);
            CARLA_SAFE_ASSERT_RETURN(engineEvents != nullptr,);

            carla_zeroStruct<EngineEvent>(engineEvents, kMaxEngineEventInternalCount);
            fillEngineEventsFromJuceMidiBuffer(engineEvents, midi);
        }

        midi.clear();

        // TODO - CV support

        const uint32_t bufferSize(static_cast<uint32_t>(audio.getNumSamples()));

        if (const int numChan = audio.getNumChannels())
        {
            if (fPlugin->getAudioInCount() == 0)
                audio.clear();

            float* audioBuffers[numChan];

            for (int i=0; i<numChan; ++i)
                audioBuffers[i] = audio.getWritePointer(i);

            float inPeaks[2] = { 0.0f };
            float outPeaks[2] = { 0.0f };

            for (int i=0; i<numChan; ++i)
            {
                for (uint32_t j=0; j < bufferSize; ++j)
                {
                    const float absV(std::abs(audioBuffers[i][j]));

                    if (absV > inPeaks[i])
                        inPeaks[i] = absV;
                }
            }

            fPlugin->process(const_cast<const float**>(audioBuffers), audioBuffers, nullptr, nullptr, bufferSize);

            for (int i=0; i<numChan; ++i)
            {
                for (uint32_t j=0; j < bufferSize; ++j)
                {
                    const float absV(std::abs(audioBuffers[i][j]));

                    if (absV > outPeaks[i])
                        outPeaks[i] = absV;
                }
            }

            engine->setPluginPeaks(fPlugin->getId(), inPeaks, outPeaks);
        }
        else
        {
            fPlugin->process(nullptr, nullptr, nullptr, nullptr, bufferSize);
        }

        midi.clear();

        if (CarlaEngineEventPort* const port = fPlugin->getDefaultEventOutPort())
        {
            /*const*/ EngineEvent* const engineEvents(port->fBuffer);
            CARLA_SAFE_ASSERT_RETURN(engineEvents != nullptr,);

            fillJuceMidiBufferFromEngineEvents(midi, engineEvents);
            carla_zeroStruct<EngineEvent>(engineEvents, kMaxEngineEventInternalCount);
        }

        fPlugin->unlock();
    }

    const String getInputChannelName(int i)  const override
    {
        CARLA_SAFE_ASSERT_RETURN(i >= 0, String());
        CarlaEngineClient* const client(fPlugin->getEngineClient());
        return client->getAudioPortName(true, static_cast<uint>(i));
    }

    const String getOutputChannelName(int i) const override
    {
        CARLA_SAFE_ASSERT_RETURN(i >= 0, String());
        CarlaEngineClient* const client(fPlugin->getEngineClient());
        return client->getAudioPortName(false, static_cast<uint>(i));
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}

    const String getParameterName(int)             override { return String(); }
          String getParameterName(int, int)        override { return String(); }
    const String getParameterText(int)             override { return String(); }
          String getParameterText(int, int)        override { return String(); }
    const String getProgramName(int)               override { return String(); }

    double getTailLengthSeconds()        const override { return 0.0;  }
    float  getParameter(int)                   override { return 0.0f; }

    bool isInputChannelStereoPair(int)   const override { return false; }
    bool isOutputChannelStereoPair(int)  const override { return false; }
    bool silenceInProducesSilenceOut()   const override { return true;  }
    bool acceptsMidi()                   const override { return fPlugin->getDefaultEventInPort()  != nullptr; }
    bool producesMidi()                  const override { return fPlugin->getDefaultEventOutPort() != nullptr; }

    void setParameter(int, float)              override {}
    void setCurrentProgram(int)                override {}
    void changeProgramName(int, const String&) override {}
    void getStateInformation(MemoryBlock&)     override {}
    void setStateInformation(const void*, int) override {}

    int getNumParameters()                     override { return 0; }
    int getNumPrograms()                       override { return 0; }
    int getCurrentProgram()                    override { return 0; }

#ifndef JUCE_AUDIO_PROCESSOR_NO_GUI
    bool hasEditor()                     const override { return false; }
    AudioProcessorEditor* createEditor()       override { return nullptr; }
#endif

    // -------------------------------------------------------------------

private:
    CarlaPlugin* const fPlugin;

    CARLA_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CarlaPluginInstance)
};

// -----------------------------------------------------------------------
// Patchbay Graph

PatchbayGraph::PatchbayGraph(const int bufferSize, const double sampleRate, const uint32_t ins, const uint32_t outs)
    : connections(),
      graph(),
      audioBuffer(),
      midiBuffer(),
      inputs(carla_fixValue(0U, MAX_PATCHBAY_PLUGINS-2, ins)),
      outputs(carla_fixValue(0U, MAX_PATCHBAY_PLUGINS-2, outs)),
      ignorePathbay(false),
      retCon()
{
    graph.setPlayConfigDetails(static_cast<int>(inputs), static_cast<int>(outputs), sampleRate, bufferSize);
    graph.prepareToPlay(sampleRate, bufferSize);

    audioBuffer.setSize(static_cast<int>(jmax(inputs, outputs)), bufferSize);

    midiBuffer.ensureSize(kMaxEngineEventInternalCount*2);
    midiBuffer.clear();

    {
        AudioProcessorGraph::AudioGraphIOProcessor* const proc(new AudioProcessorGraph::AudioGraphIOProcessor(AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));
        AudioProcessorGraph::Node* const node(graph.addNode(proc));
        node->properties.set("isPlugin", false);
        node->properties.set("isOutput", false);
        node->properties.set("isAudio", true);
        node->properties.set("isMIDI", false);
    }

    {
        AudioProcessorGraph::AudioGraphIOProcessor* const proc(new AudioProcessorGraph::AudioGraphIOProcessor(AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
        AudioProcessorGraph::Node* const node(graph.addNode(proc));
        node->properties.set("isPlugin", false);
        node->properties.set("isOutput", false);
        node->properties.set("isAudio", true);
        node->properties.set("isMIDI", false);
    }

    {
        AudioProcessorGraph::AudioGraphIOProcessor* const proc(new AudioProcessorGraph::AudioGraphIOProcessor(AudioProcessorGraph::AudioGraphIOProcessor::midiInputNode));
        AudioProcessorGraph::Node* const node(graph.addNode(proc));
        node->properties.set("isPlugin", false);
        node->properties.set("isOutput", false);
        node->properties.set("isAudio", false);
        node->properties.set("isMIDI", true);
    }

    {
        AudioProcessorGraph::AudioGraphIOProcessor* const proc(new AudioProcessorGraph::AudioGraphIOProcessor(AudioProcessorGraph::AudioGraphIOProcessor::midiOutputNode));
        AudioProcessorGraph::Node* const node(graph.addNode(proc));
        node->properties.set("isPlugin", false);
        node->properties.set("isOutput", true);
        node->properties.set("isAudio", false);
        node->properties.set("isMIDI", true);
    }
}

PatchbayGraph::~PatchbayGraph()
{
    clearConnections();
    graph.releaseResources();
    graph.clear();
    audioBuffer.clear();
}

void PatchbayGraph::setBufferSize(const int bufferSize)
{
    graph.releaseResources();
    graph.prepareToPlay(graph.getSampleRate(), bufferSize);
    audioBuffer.setSize(audioBuffer.getNumChannels(), bufferSize);
}

void PatchbayGraph::setSampleRate(const double sampleRate)
{
    graph.releaseResources();
    graph.prepareToPlay(sampleRate, graph.getBlockSize());
}

void PatchbayGraph::setOffline(const bool offline)
{
    graph.setNonRealtime(offline);
}

void PatchbayGraph::addPlugin(CarlaPlugin* const plugin)
{
    CARLA_SAFE_ASSERT_RETURN(plugin != nullptr,);
    carla_debug("PatchbayGraph::addPlugin(%p)", plugin);

    CarlaPluginInstance* const instance(new CarlaPluginInstance(plugin));
    AudioProcessorGraph::Node* const node(graph.addNode(instance));
    CARLA_SAFE_ASSERT_RETURN(node != nullptr,);

    plugin->setPatchbayNodeId(node->nodeId);

    node->properties.set("isPlugin", true);
    node->properties.set("pluginId", static_cast<int>(plugin->getId()));

    if (! ignorePathbay)
        addNodeToPatchbay(plugin->getEngine(), node->nodeId, static_cast<int>(plugin->getId()), instance);
}

void PatchbayGraph::replacePlugin(CarlaPlugin* const oldPlugin, CarlaPlugin* const newPlugin)
{
    CARLA_SAFE_ASSERT_RETURN(oldPlugin != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(newPlugin != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(oldPlugin != newPlugin,);
    CARLA_SAFE_ASSERT_RETURN(oldPlugin->getId() == newPlugin->getId(),);

    CarlaEngine* const engine(oldPlugin->getEngine());
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);

    AudioProcessorGraph::Node* const oldNode(graph.getNodeForId(oldPlugin->getPatchbayNodeId()));
    CARLA_SAFE_ASSERT_RETURN(oldNode != nullptr,);

    if (! ignorePathbay)
    {
        disconnectGroup(engine, oldNode->nodeId);
        removeNodeFromPatchbay(engine, oldNode->nodeId, oldNode->getProcessor());
    }

    graph.removeNode(oldNode->nodeId);

    CarlaPluginInstance* const instance(new CarlaPluginInstance(newPlugin));
    AudioProcessorGraph::Node* const node(graph.addNode(instance));
    CARLA_SAFE_ASSERT_RETURN(node != nullptr,);

    newPlugin->setPatchbayNodeId(node->nodeId);

    node->properties.set("isPlugin", true);
    node->properties.set("pluginId", static_cast<int>(newPlugin->getId()));

    if (! ignorePathbay)
        addNodeToPatchbay(newPlugin->getEngine(), node->nodeId, static_cast<int>(newPlugin->getId()), instance);
}

void PatchbayGraph::removePlugin(CarlaPlugin* const plugin)
{
    CARLA_SAFE_ASSERT_RETURN(plugin != nullptr,);
    carla_debug("PatchbayGraph::removePlugin(%p)", plugin);

    CarlaEngine* const engine(plugin->getEngine());
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);

    AudioProcessorGraph::Node* const node(graph.getNodeForId(plugin->getPatchbayNodeId()));
    CARLA_SAFE_ASSERT_RETURN(node != nullptr,);

    if (! ignorePathbay)
    {
        disconnectGroup(engine, node->nodeId);
        removeNodeFromPatchbay(engine, node->nodeId, node->getProcessor());
    }

    // Fix plugin Ids properties
    for (uint i=plugin->getId()+1, count=engine->getCurrentPluginCount(); i<count; ++i)
    {
        CarlaPlugin* const plugin2(engine->getPlugin(i));
        CARLA_SAFE_ASSERT_BREAK(plugin2 != nullptr);

        if (AudioProcessorGraph::Node* const node2 = graph.getNodeForId(plugin2->getPatchbayNodeId()))
        {
            CARLA_SAFE_ASSERT_CONTINUE(node2->properties.getWithDefault("pluginId", -1) != juce::var(-1));
            node2->properties.set("pluginId", static_cast<int>(i-1));
        }
    }

    CARLA_SAFE_ASSERT_RETURN(graph.removeNode(node->nodeId),);
}

void PatchbayGraph::removeAllPlugins(CarlaEngine* const engine)
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);
    carla_debug("PatchbayGraph::removeAllPlugins(%p)", engine);

    for (uint i=0, count=engine->getCurrentPluginCount(); i<count; ++i)
    {
        CarlaPlugin* const plugin(engine->getPlugin(i));
        CARLA_SAFE_ASSERT_CONTINUE(plugin != nullptr);

        AudioProcessorGraph::Node* const node(graph.getNodeForId(plugin->getPatchbayNodeId()));
        CARLA_SAFE_ASSERT_CONTINUE(node != nullptr);

        if (! ignorePathbay)
        {
            disconnectGroup(engine, node->nodeId);
            removeNodeFromPatchbay(engine, node->nodeId, node->getProcessor());
        }

        graph.removeNode(node->nodeId);
    }
}

bool PatchbayGraph::connect(CarlaEngine* const engine, const uint groupA, const uint portA, const uint groupB, const uint portB) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr, false);

    uint adjustedPortA = portA;
    uint adjustedPortB = portB;

    if (! adjustPatchbayPortIdForJuce(adjustedPortA))
        return false;
    if (! adjustPatchbayPortIdForJuce(adjustedPortB))
        return false;

    if (! graph.addConnection(groupA, static_cast<int>(adjustedPortA), groupB, static_cast<int>(adjustedPortB)))
    {
        engine->setLastError("Failed from juce");
        return false;
    }

    ConnectionToId connectionToId;
    connectionToId.setData(++connections.lastId, groupA, portA, groupB, portB);

    char strBuf[STR_MAX+1];
    strBuf[STR_MAX] = '\0';
    std::snprintf(strBuf, STR_MAX, "%u:%u:%u:%u", groupA, portA, groupB, portB);

    engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, connectionToId.id, 0, 0, 0.0f, strBuf);

    connections.list.append(connectionToId);
    return true;
}

bool PatchbayGraph::disconnect(CarlaEngine* const engine, const uint connectionId) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr, false);

    for (LinkedList<ConnectionToId>::Itenerator it=connections.list.begin(); it.valid(); it.next())
    {
        static const ConnectionToId fallback = { 0, 0, 0, 0, 0 };

        const ConnectionToId& connectionToId(it.getValue(fallback));
        CARLA_SAFE_ASSERT_CONTINUE(connectionToId.id != 0);

        if (connectionToId.id != connectionId)
            continue;

        uint adjustedPortA = connectionToId.portA;
        uint adjustedPortB = connectionToId.portB;

        if (! adjustPatchbayPortIdForJuce(adjustedPortA))
            return false;
        if (! adjustPatchbayPortIdForJuce(adjustedPortB))
            return false;

        if (! graph.removeConnection(connectionToId.groupA, static_cast<int>(adjustedPortA),
                                     connectionToId.groupB, static_cast<int>(adjustedPortB)))
            return false;

        engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED, connectionToId.id, 0, 0, 0.0f, nullptr);

        connections.list.remove(it);
        return true;
    }

    engine->setLastError("Failed to find connection");
    return false;
}

void PatchbayGraph::disconnectGroup(CarlaEngine* const engine, const uint groupId) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);

    for (LinkedList<ConnectionToId>::Itenerator it=connections.list.begin(); it.valid(); it.next())
    {
        static const ConnectionToId fallback = { 0, 0, 0, 0, 0 };

        const ConnectionToId& connectionToId(it.getValue(fallback));
        CARLA_SAFE_ASSERT_CONTINUE(connectionToId.id != 0);

        if (connectionToId.groupA != groupId && connectionToId.groupB != groupId)
            continue;

        /*
        uint adjustedPortA = connectionToId.portA;
        uint adjustedPortB = connectionToId.portB;

        if (! adjustPatchbayPortIdForJuce(adjustedPortA))
            return false;
        if (! adjustPatchbayPortIdForJuce(adjustedPortB))
            return false;

        graph.removeConnection(connectionToId.groupA, static_cast<int>(adjustedPortA),
                               connectionToId.groupB, static_cast<int>(adjustedPortB));
        */

        engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED, connectionToId.id, 0, 0, 0.0f, nullptr);

        connections.list.remove(it);
    }
}

void PatchbayGraph::clearConnections()
{
    connections.clear();

    for (int i=0, count=graph.getNumConnections(); i<count; ++i)
        graph.removeConnection(0);
}

void PatchbayGraph::refreshConnections(CarlaEngine* const engine)
{
    CARLA_SAFE_ASSERT_RETURN(engine != nullptr,);

    connections.clear();
    graph.removeIllegalConnections();

    for (int i=0, count=graph.getNumNodes(); i<count; ++i)
    {
        AudioProcessorGraph::Node* const node(graph.getNode(i));
        CARLA_SAFE_ASSERT_CONTINUE(node != nullptr);

        AudioProcessor* const proc(node->getProcessor());
        CARLA_SAFE_ASSERT_CONTINUE(proc != nullptr);

        int clientId = -1;

        if (node->properties.getWithDefault("isPlugin", false) == juce::var(true))
            clientId = node->properties.getWithDefault("pluginId", -1);

        if (! ignorePathbay)
            addNodeToPatchbay(engine, node->nodeId, clientId, proc);
    }

    char strBuf[STR_MAX+1];

    for (int i=0, count=graph.getNumConnections(); i<count; ++i)
    {
        const AudioProcessorGraph::Connection* const conn(graph.getConnection(i));
        CARLA_SAFE_ASSERT_CONTINUE(conn != nullptr);
        CARLA_SAFE_ASSERT_CONTINUE(conn->sourceChannelIndex >= 0);
        CARLA_SAFE_ASSERT_CONTINUE(conn->destChannelIndex >= 0);

        const uint groupA = conn->sourceNodeId;
        const uint groupB = conn->destNodeId;

        uint portA = static_cast<uint>(conn->sourceChannelIndex);
        uint portB = static_cast<uint>(conn->destChannelIndex);

        if (portA == kMidiChannelIndex)
            portA  = kMidiOutputPortOffset;
        else
            portA += kAudioOutputPortOffset;

        if (portB == kMidiChannelIndex)
            portB  = kMidiInputPortOffset;
        else
            portB += kAudioInputPortOffset;

        ConnectionToId connectionToId;
        connectionToId.setData(++connections.lastId, groupA, portA, groupB, portB);

        std::snprintf(strBuf, STR_MAX, "%i:%i:%i:%i", groupA, portA, groupB, portB);
        strBuf[STR_MAX] = '\0';

        engine->callback(ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, connectionToId.id, 0, 0, 0.0f, strBuf);

        connections.list.append(connectionToId);
    }
}

const char* const* PatchbayGraph::getConnections() const
{
    if (connections.list.count() == 0)
        return nullptr;

    CarlaStringList connList;

    for (LinkedList<ConnectionToId>::Itenerator it=connections.list.begin(); it.valid(); it.next())
    {
        static const ConnectionToId fallback = { 0, 0, 0, 0, 0 };

        const ConnectionToId& connectionToId(it.getValue(fallback));
        CARLA_SAFE_ASSERT_CONTINUE(connectionToId.id != 0);

        AudioProcessorGraph::Node* const nodeA(graph.getNodeForId(connectionToId.groupA));
        CARLA_SAFE_ASSERT_CONTINUE(nodeA != nullptr);

        AudioProcessorGraph::Node* const nodeB(graph.getNodeForId(connectionToId.groupB));
        CARLA_SAFE_ASSERT_CONTINUE(nodeB != nullptr);

        AudioProcessor* const procA(nodeA->getProcessor());
        CARLA_SAFE_ASSERT_CONTINUE(procA != nullptr);

        AudioProcessor* const procB(nodeB->getProcessor());
        CARLA_SAFE_ASSERT_CONTINUE(procB != nullptr);

        String fullPortNameA(getProcessorFullPortName(procA, connectionToId.portA));
        CARLA_SAFE_ASSERT_CONTINUE(fullPortNameA.isNotEmpty());

        String fullPortNameB(getProcessorFullPortName(procB, connectionToId.portB));
        CARLA_SAFE_ASSERT_CONTINUE(fullPortNameB.isNotEmpty());

        connList.append(fullPortNameA.toRawUTF8());
        connList.append(fullPortNameB.toRawUTF8());
    }

    if (connList.count() == 0)
        return nullptr;

    retCon = connList.toCharStringListPtr();

    return retCon;
}

bool PatchbayGraph::getGroupAndPortIdFromFullName(const char* const fullPortName, uint& groupId, uint& portId) const
{
    String groupName(String(fullPortName).upToFirstOccurrenceOf(":", false, false));
    String portName(String(fullPortName).fromFirstOccurrenceOf(":", false, false));

    for (int i=0, count=graph.getNumNodes(); i<count; ++i)
    {
        AudioProcessorGraph::Node* const node(graph.getNode(i));
        CARLA_SAFE_ASSERT_CONTINUE(node != nullptr);

        AudioProcessor* const proc(node->getProcessor());
        CARLA_SAFE_ASSERT_CONTINUE(proc != nullptr);

        if (proc->getName() != groupName)
            continue;

        groupId = node->nodeId;

        if (portName == "events-in")
        {
            portId = kMidiInputPortOffset;
            return true;
        }
        if (portName == "events-out")
        {
            portId = kMidiOutputPortOffset;
            return true;
        }
        else
        {
            for (int j=0, numInputs=proc->getNumInputChannels(); j<numInputs; ++j)
            {
                if (proc->getInputChannelName(j) != portName)
                    continue;

                portId = kAudioInputPortOffset+static_cast<uint>(j);
                return true;
            }

            for (int j=0, numOutputs=proc->getNumOutputChannels(); j<numOutputs; ++j)
            {
                if (proc->getOutputChannelName(j) != portName)
                    continue;

                portId = kAudioOutputPortOffset+static_cast<uint>(j);
                return true;
            }
        }
    }

    return false;
}

void PatchbayGraph::process(CarlaEngine::ProtectedData* const data, const float* const* const inBuf, float* const* const outBuf, const int frames)
{
    CARLA_SAFE_ASSERT_RETURN(data != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(data->events.in != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(data->events.out != nullptr,);
    CARLA_SAFE_ASSERT_RETURN(frames > 0,);

    // put events in juce buffer
    {
        midiBuffer.clear();
        fillJuceMidiBufferFromEngineEvents(midiBuffer, data->events.in);
    }

    // put carla audio in juce buffer
    {
        int i=0;

        for (; i < static_cast<int>(inputs); ++i)
            FloatVectorOperations::copy(audioBuffer.getWritePointer(i), inBuf[i], frames);

        // clear remaining channels
        for (const int count=audioBuffer.getNumChannels(); i<count; ++i)
            audioBuffer.clear(i, 0, frames);
    }

    graph.processBlock(audioBuffer, midiBuffer);

    // put juce audio in carla buffer
    {
        for (int i=0; i < static_cast<int>(outputs); ++i)
            FloatVectorOperations::copy(outBuf[i], audioBuffer.getReadPointer(i), frames);
    }

    // put juce events in carla buffer
    {
        carla_zeroStruct<EngineEvent>(data->events.out, kMaxEngineEventInternalCount);
        fillEngineEventsFromJuceMidiBuffer(data->events.out, midiBuffer);
        midiBuffer.clear();
    }
}

// -----------------------------------------------------------------------
// InternalGraph

EngineInternalGraph::EngineInternalGraph() noexcept
    : fIsRack(true),
      fIsReady(false)
{
    fRack = nullptr;
}

EngineInternalGraph::~EngineInternalGraph() noexcept
{
    CARLA_SAFE_ASSERT(! fIsReady);
    CARLA_SAFE_ASSERT(fRack == nullptr);
}

void EngineInternalGraph::create(const bool isRack, const double sampleRate, const uint32_t bufferSize, const uint32_t inputs, const uint32_t outputs)
{
    fIsRack = isRack;

    if (isRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack == nullptr,);
        fRack = new RackGraph(bufferSize, inputs, outputs);
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay == nullptr,);
        fPatchbay = new PatchbayGraph(static_cast<int>(bufferSize), sampleRate, inputs, outputs);
    }

    fIsReady = true;
}

void EngineInternalGraph::destroy() noexcept
{
    if (! fIsReady)
    {
        CARLA_SAFE_ASSERT(fRack == nullptr);
        return;
    }

    fIsReady = false;

    if (fIsRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);
        delete fRack;
        fRack = nullptr;
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
        delete fPatchbay;
        fPatchbay = nullptr;
    }
}

void EngineInternalGraph::setBufferSize(const uint32_t bufferSize)
{
    ScopedValueSetter<bool> svs(fIsReady, false, true);

    if (fIsRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);
        fRack->setBufferSize(bufferSize);
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
        fPatchbay->setBufferSize(static_cast<int>(bufferSize));
    }
}

void EngineInternalGraph::setSampleRate(const double sampleRate)
{
    ScopedValueSetter<bool> svs(fIsReady, false, true);

    if (fIsRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
        fPatchbay->setSampleRate(sampleRate);
    }
}

void EngineInternalGraph::setOffline(const bool offline)
{
    ScopedValueSetter<bool> svs(fIsReady, false, true);

    if (fIsRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);
        fRack->setOffline(offline);
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
        fPatchbay->setOffline(offline);
    }
}

bool EngineInternalGraph::isReady() const noexcept
{
    return fIsReady;
}

RackGraph* EngineInternalGraph::getRackGraph() const noexcept
{
    CARLA_SAFE_ASSERT_RETURN(fIsRack, nullptr);

    return fRack;
}

PatchbayGraph* EngineInternalGraph::getPatchbayGraph() const noexcept
{
    CARLA_SAFE_ASSERT_RETURN(! fIsRack, nullptr);

    return fPatchbay;
}

void EngineInternalGraph::process(CarlaEngine::ProtectedData* const data, const float* const* const inBuf, float* const* const outBuf, const uint32_t frames)
{
    if (fIsRack)
    {
        CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);
        fRack->processHelper(data, inBuf, outBuf, frames);
    }
    else
    {
        CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
        fPatchbay->process(data, inBuf, outBuf, static_cast<int>(frames));
    }
}

void EngineInternalGraph::processRack(CarlaEngine::ProtectedData* const data, const float* inBuf[2], float* outBuf[2], const uint32_t frames)
{
    CARLA_SAFE_ASSERT_RETURN(fIsRack,);
    CARLA_SAFE_ASSERT_RETURN(fRack != nullptr,);

    fRack->process(data, inBuf, outBuf, frames);
}

// -----------------------------------------------------------------------
// used for internal patchbay mode

void EngineInternalGraph::addPlugin(CarlaPlugin* const plugin)
{
    CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
    fPatchbay->addPlugin(plugin);
}

void EngineInternalGraph::replacePlugin(CarlaPlugin* const oldPlugin, CarlaPlugin* const newPlugin)
{
    CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
    fPatchbay->replacePlugin(oldPlugin, newPlugin);
}

void EngineInternalGraph::removePlugin(CarlaPlugin* const plugin)
{
    CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
    fPatchbay->removePlugin(plugin);
}

void EngineInternalGraph::removeAllPlugins(CarlaEngine* const engine)
{
    CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
    fPatchbay->removeAllPlugins(engine);
}

void EngineInternalGraph::setIgnorePatchbay(const bool ignore) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(fPatchbay != nullptr,);
    fPatchbay->ignorePathbay = ignore;
}

// -----------------------------------------------------------------------
// CarlaEngine Patchbay stuff

bool CarlaEngine::patchbayConnect(const uint groupA, const uint portA, const uint groupB, const uint portB)
{
    CARLA_SAFE_ASSERT_RETURN(pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK || pData->options.processMode == ENGINE_PROCESS_MODE_PATCHBAY, false);
    CARLA_SAFE_ASSERT_RETURN(pData->graph.isReady(), false);
    carla_debug("CarlaEngine::patchbayConnect(%u, %u, %u, %u)", groupA, portA, groupB, portB);

    if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        if (RackGraph* const graph = pData->graph.getRackGraph())
            return graph->connect(this, groupA, portA, groupB, portB);
    }
    else
    {
        if (PatchbayGraph* const graph = pData->graph.getPatchbayGraph())
            return graph->connect(this, groupA, portA, groupB, portB);
    }

    return false;
}

bool CarlaEngine::patchbayDisconnect(const uint connectionId)
{
    CARLA_SAFE_ASSERT_RETURN(pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK || pData->options.processMode == ENGINE_PROCESS_MODE_PATCHBAY, false);
    CARLA_SAFE_ASSERT_RETURN(pData->graph.isReady(), false);
    carla_debug("CarlaEngine::patchbayDisconnect(%u)", connectionId);

    if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        if (RackGraph* const graph = pData->graph.getRackGraph())
            return graph->disconnect(this, connectionId);
    }
    else
    {
        if (PatchbayGraph* const graph = pData->graph.getPatchbayGraph())
            return graph->disconnect(this, connectionId);
    }

    return false;
}

bool CarlaEngine::patchbayRefresh(const bool external)
{
    // subclasses should handle this
    CARLA_SAFE_ASSERT_RETURN(! external, false);

    if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        // This is implemented in engine subclasses for MIDI support
        setLastError("Unsupported operation");
        return false;
    }

    CARLA_SAFE_ASSERT_RETURN(pData->options.processMode == ENGINE_PROCESS_MODE_PATCHBAY, false);

    PatchbayGraph* const graph = pData->graph.getPatchbayGraph();
    CARLA_SAFE_ASSERT_RETURN(graph != nullptr, false);

    graph->refreshConnections(this);

    return true;
}

// -----------------------------------------------------------------------

const char* const* CarlaEngine::getPatchbayConnections() const
{
    CARLA_SAFE_ASSERT_RETURN(pData->graph.isReady(), nullptr);
    carla_debug("CarlaEngine::getPatchbayConnections()");

    if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        if (RackGraph* const graph = pData->graph.getRackGraph())
            return graph->getConnections();
    }
    else
    {
        if (PatchbayGraph* const graph = pData->graph.getPatchbayGraph())
            return graph->getConnections();
    }

    return nullptr;
}

void CarlaEngine::restorePatchbayConnection(const char* const connSource, const char* const connTarget)
{
    CARLA_SAFE_ASSERT_RETURN(pData->graph.isReady(),);
    CARLA_SAFE_ASSERT_RETURN(connSource != nullptr && connSource[0] != '\0',);
    CARLA_SAFE_ASSERT_RETURN(connTarget != nullptr && connTarget[0] != '\0',);
    carla_debug("CarlaEngine::restorePatchbayConnection(\"%s\", \"%s\")", connSource, connTarget);

    uint groupA, portA;
    uint groupB, portB;

    if (pData->options.processMode == ENGINE_PROCESS_MODE_CONTINUOUS_RACK)
    {
        RackGraph* const graph = pData->graph.getRackGraph();
        CARLA_SAFE_ASSERT_RETURN(graph != nullptr,);

        if (! graph->getGroupAndPortIdFromFullName(connSource, groupA, portA))
            return;
        if (! graph->getGroupAndPortIdFromFullName(connTarget, groupB, portB))
            return;
    }
    else
    {
        PatchbayGraph* const graph = pData->graph.getPatchbayGraph();
        CARLA_SAFE_ASSERT_RETURN(graph != nullptr,);

        if (! graph->getGroupAndPortIdFromFullName(connSource, groupA, portA))
            return;
        if (! graph->getGroupAndPortIdFromFullName(connTarget, groupB, portB))
            return;
    }

    patchbayConnect(groupA, portA, groupB, portB);
}

// -----------------------------------------------------------------------

CARLA_BACKEND_END_NAMESPACE

// -----------------------------------------------------------------------
