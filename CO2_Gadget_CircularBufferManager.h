// clang-format off
/*****************************************************************************************************/
/*********                                                                                   *********/
/*********                          SETUP CIRCULAR BUFFER FUNCTIONALITY                      *********/
/*********                                                                                   *********/
/*****************************************************************************************************/
/*****************************************************************************************************/
// clang-format on

#ifndef CO2_Gadget_CircularBufferManager_h
#define CO2_Gadget_CircularBufferManager_h

#include <Arduino.h>
#include <Preferences.h>

#include <vector>

// Default sampling interval in seconds (default: 10 seconds)
#ifndef CIRCULAR_BUFFER_SAMPLE_INTERVAL_S
#define CIRCULAR_BUFFER_SAMPLE_INTERVAL_S 10
#endif
uint16_t sensorSampleInterval = CIRCULAR_BUFFER_SAMPLE_INTERVAL_S;

// Default interval for moving average calculation in seconds (default: 60 seconds)
#ifndef CIRCULAR_BUFFER_AVG_INTERVAL_S
#define CIRCULAR_BUFFER_AVG_INTERVAL_S 60
#endif
uint16_t movingAverageInterval = CIRCULAR_BUFFER_AVG_INTERVAL_S;

// Default capacity for the long-term buffer (storing one day of data with 1-minute intervals)
#ifndef CIRCULAR_BUFFER_CAPACITY
#define CIRCULAR_BUFFER_CAPACITY 1440
#endif
uint16_t longTermBufferCapacity = CIRCULAR_BUFFER_CAPACITY;

// Buffers to store sensor data for moving average calculation and long-term storage
std::vector<uint16_t> sensorDataBuffer;
std::vector<uint16_t> longTermBuffer;

// Variables to keep track of the last sample time and last average
uint64_t lastSampleTime = 0;
uint16_t lastAverage = 0;
uint16_t samplesPerInterval = movingAverageInterval / sensorSampleInterval;

// NVS persistence for the long-term CO2 circular buffer.
// Uses a chunked ring buffer in the dedicated "CO2-Hist" NVS namespace so each
// logged interval only rewrites one 64-byte chunk + 12-byte metadata instead of
// the full buffer, minimizing flash wear while surviving power outages.
static const char* HISTORY_NVS_NAMESPACE = "CO2-Hist";
static constexpr uint32_t HISTORY_NVS_MAGIC = 0x434F3248;  // "CO2H"
static constexpr uint16_t HISTORY_NVS_VERSION = 1;
static constexpr uint16_t HISTORY_CHUNK_SIZE = 32;  // 32 * uint16_t = 64 bytes per chunk

struct HistoryNvsMeta {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint16_t head;
    uint16_t count;
};

Preferences historyPreferences;
HistoryNvsMeta historyMeta = {HISTORY_NVS_MAGIC, HISTORY_NVS_VERSION, CIRCULAR_BUFFER_CAPACITY, 0, 0};
bool circularBufferInitialized = false;

String getHistoryChunkKey(uint16_t chunkIndex) {
    return "c" + String(chunkIndex);
}

bool saveHistoryMetaToNVS() {
    return historyPreferences.putBytes("meta", &historyMeta, sizeof(historyMeta)) == sizeof(historyMeta);
}

void resetHistoryNVS(uint16_t capacity) {
    historyPreferences.clear();
    historyMeta = {HISTORY_NVS_MAGIC, HISTORY_NVS_VERSION, capacity, 0, 0};
    saveHistoryMetaToNVS();
}

void loadHistoryFromNVS() {
    if (longTermBufferCapacity == 0) {
        return;
    }
    if (!historyPreferences.begin(HISTORY_NVS_NAMESPACE, false)) {
        Serial.println("-->[BUFF][ERR] Failed to open NVS namespace for CO2 history");
        return;
    }

    HistoryNvsMeta loadedMeta = {};
    size_t metaLen = historyPreferences.getBytes("meta", &loadedMeta, sizeof(loadedMeta));
    bool metaValid = (metaLen == sizeof(HistoryNvsMeta)) &&
                     (loadedMeta.magic == HISTORY_NVS_MAGIC) &&
                     (loadedMeta.version == HISTORY_NVS_VERSION) &&
                     (loadedMeta.capacity == longTermBufferCapacity) &&
                     (loadedMeta.count <= loadedMeta.capacity) &&
                     (loadedMeta.head < loadedMeta.capacity);

    if (!metaValid) {
        Serial.println("-->[BUFF] No valid NVS CO2 history found; initializing empty log");
        longTermBuffer.clear();
        resetHistoryNVS(longTermBufferCapacity);
        historyPreferences.end();
        return;
    }

    historyMeta = loadedMeta;
    longTermBuffer.clear();

    if (historyMeta.count > 0) {
        std::vector<uint16_t> ring(historyMeta.capacity, 0);
        uint16_t numChunks = (historyMeta.capacity + HISTORY_CHUNK_SIZE - 1) / HISTORY_CHUNK_SIZE;
        uint16_t chunk[HISTORY_CHUNK_SIZE];

        for (uint16_t c = 0; c < numChunks; c++) {
            memset(chunk, 0, sizeof(chunk));
            String key = getHistoryChunkKey(c);
            historyPreferences.getBytes(key.c_str(), chunk, sizeof(chunk));
            uint16_t baseIdx = c * HISTORY_CHUNK_SIZE;
            for (uint16_t j = 0; j < HISTORY_CHUNK_SIZE && (baseIdx + j) < historyMeta.capacity; j++) {
                ring[baseIdx + j] = chunk[j];
            }
        }

        uint16_t startIdx = (historyMeta.head + historyMeta.capacity - historyMeta.count) % historyMeta.capacity;
        for (uint16_t i = 0; i < historyMeta.count; i++) {
            longTermBuffer.push_back(ring[(startIdx + i) % historyMeta.capacity]);
        }
        lastAverage = longTermBuffer.back();
        Serial.println("-->[BUFF] Restored " + String(longTermBuffer.size()) + " CO2 history samples from NVS");
    }

    historyPreferences.end();
}

void persistCO2SampleToNVS(uint16_t newValue) {
    if (longTermBufferCapacity == 0) {
        return;
    }
    if (!historyPreferences.begin(HISTORY_NVS_NAMESPACE, false)) {
        Serial.println("-->[BUFF][ERR] Failed to open NVS to persist CO2 sample");
        return;
    }

    uint16_t slot = historyMeta.head;
    uint16_t chunkIdx = slot / HISTORY_CHUNK_SIZE;
    uint16_t offset = slot % HISTORY_CHUNK_SIZE;
    uint16_t chunk[HISTORY_CHUNK_SIZE] = {0};

    String key = getHistoryChunkKey(chunkIdx);
    historyPreferences.getBytes(key.c_str(), chunk, sizeof(chunk));
    chunk[offset] = newValue;

    if (historyPreferences.putBytes(key.c_str(), chunk, sizeof(chunk)) == sizeof(chunk)) {
        historyMeta.head = (historyMeta.head + 1) % historyMeta.capacity;
        if (historyMeta.count < historyMeta.capacity) {
            historyMeta.count++;
        }
        saveHistoryMetaToNVS();
    } else {
        Serial.println("-->[BUFF][ERR] Failed to write CO2 history chunk to NVS");
    }

    historyPreferences.end();
}

void syncFullHistoryToNVS() {
    if (longTermBufferCapacity == 0) {
        return;
    }
    while (longTermBuffer.size() > longTermBufferCapacity) {
        longTermBuffer.erase(longTermBuffer.begin());
    }
    if (!historyPreferences.begin(HISTORY_NVS_NAMESPACE, false)) {
        return;
    }

    historyPreferences.clear();
    uint16_t count = static_cast<uint16_t>(longTermBuffer.size());
    uint16_t numChunks = (count + HISTORY_CHUNK_SIZE - 1) / HISTORY_CHUNK_SIZE;
    uint16_t chunk[HISTORY_CHUNK_SIZE];

    for (uint16_t c = 0; c < numChunks; c++) {
        memset(chunk, 0, sizeof(chunk));
        uint16_t baseIdx = c * HISTORY_CHUNK_SIZE;
        for (uint16_t j = 0; j < HISTORY_CHUNK_SIZE && (baseIdx + j) < count; j++) {
            chunk[j] = longTermBuffer[baseIdx + j];
        }
        String key = getHistoryChunkKey(c);
        historyPreferences.putBytes(key.c_str(), chunk, sizeof(chunk));
    }

    historyMeta = {
        HISTORY_NVS_MAGIC,
        HISTORY_NVS_VERSION,
        longTermBufferCapacity,
        static_cast<uint16_t>(count % longTermBufferCapacity),
        count
    };
    saveHistoryMetaToNVS();
    historyPreferences.end();
}

// Function to calculate the moving average of a vector buffer
uint16_t calculateMovingAverage(const std::vector<uint16_t>& buffer) {
    uint32_t sum = 0;
    size_t count = buffer.size();

#ifdef DEBUG_CIRCULAR_BUFFER
    if (!inMenu) {
        Serial.print("-->[BUFF] Calculating moving average. Buffer size: ");
        Serial.println(count);
    }
#endif

    for (size_t i = 0; i < count; i++) {
        sum += buffer[i];
#ifdef DEBUG_CIRCULAR_BUFFER
        if (!inMenu) {
            Serial.print("-->[BUFF] Adding value: ");
            Serial.println(buffer[i]);
        }
#endif
    }

    if (count == 0) {
        return 0;
    }

    return static_cast<uint16_t>((sum + count / 2) / count);
}

// Function to update the buffer with a new value based on the interval
void updateBuffer() {
    if (millis() - lastSampleTime >= static_cast<uint32_t>(movingAverageInterval) * 1000UL) {
        uint16_t newValue;
        if (sensorDataBuffer.empty()) {
            newValue = lastAverage;  // Use the last average if no new data was received
#ifdef DEBUG_CIRCULAR_BUFFER
            if (!inMenu) {
                Serial.println("-->[BUFF] No new data received. Using last average.");
            }
#endif
        } else {
            newValue = calculateMovingAverage(sensorDataBuffer);
#ifdef DEBUG_CIRCULAR_BUFFER
            if (!inMenu) {
                Serial.print("-->[BUFF] New average calculated: ");
                Serial.println(newValue);
            }
#endif
        }

        // Update the last average
        lastAverage = newValue;

        // Store the calculated average in the long-term buffer
        if (longTermBuffer.size() >= longTermBufferCapacity) {
            longTermBuffer.erase(longTermBuffer.begin());  // Remove the oldest value if the buffer is full
#ifdef DEBUG_CIRCULAR_BUFFER
            if (!inMenu) {
                Serial.println("-->[BUFF] Long-term buffer full. Removing oldest value.");
            }
#endif
        }
        longTermBuffer.push_back(newValue);
        persistCO2SampleToNVS(newValue);

        // Clear the buffer for the next interval
        sensorDataBuffer.clear();

        // Update the last sample time
        lastSampleTime = millis();
#ifdef DEBUG_CIRCULAR_BUFFER
        if (!inMenu) {
            Serial.print("-->[BUFF] Interval updated. New average: ");
            Serial.println(newValue);
            Serial.print("-->[BUFF] Data points in long-term buffer: ");
            Serial.println(longTermBuffer.size());
            // Print the entire long-term buffer
            Serial.println("-->[BUFF] Long-term buffer values:");
            for (size_t i = 0; i < longTermBuffer.size(); i++) {
                Serial.print(longTermBuffer[i]);
                Serial.print(" ");
            }
            Serial.println();
        }
#endif
    }
}

void initCircularBuffer();

// Function to add a new CO2 value to the buffer
void addCO2Value(uint16_t co2) {
    if (!circularBufferInitialized) {
        initCircularBuffer();
    }
    if (sensorDataBuffer.size() >= samplesPerInterval) {
        // Remove the oldest value to make space for the new value
        sensorDataBuffer.erase(sensorDataBuffer.begin());
#ifdef DEBUG_CIRCULAR_BUFFER
        if (!inMenu) {
            Serial.println("-->[BUFF] Buffer full. Removing oldest CO2 value.");
        }
#endif
    }

    sensorDataBuffer.push_back(co2);
#ifdef DEBUG_CIRCULAR_BUFFER
    if (!inMenu) {
        Serial.print("-->[BUFF] Added new CO2 value: ");
        Serial.println(co2);
        Serial.print("-->[BUFF] Current sensor samples buffer size: ");
        Serial.println(sensorDataBuffer.size());
    }
#endif

    // Update the buffer with the new value
    updateBuffer();
}

// Function to set the sampling interval in seconds
void setSensorSampleInterval(uint16_t intervalSeconds) {
    sensorSampleInterval = intervalSeconds;
    samplesPerInterval = movingAverageInterval / sensorSampleInterval;
#ifdef DEBUG_CIRCULAR_BUFFER
    if (!inMenu) {
        Serial.print("-->[BUFF] Sensor sample interval set to ");
        Serial.print(intervalSeconds);
        Serial.println(" seconds");
    }
#endif
}

// Function to set the moving average interval in seconds
void setMovingAverageInterval(uint16_t intervalSeconds) {
    movingAverageInterval = intervalSeconds;
    samplesPerInterval = movingAverageInterval / sensorSampleInterval;
#ifdef DEBUG_CIRCULAR_BUFFER
    if (!inMenu) {
        Serial.print("-->[BUFF] Moving average interval set to ");
        Serial.print(intervalSeconds);
        Serial.println(" seconds");
    }
#endif
}

// Function to set the capacity of the long-term buffer
void setLongTermBufferCapacity(uint16_t capacity) {
    longTermBufferCapacity = capacity;
#ifdef DEBUG_CIRCULAR_BUFFER
    if (!inMenu) {
        Serial.print("-->[BUFF] Long-term buffer capacity set to ");
        Serial.println(capacity);
    }
#endif
}

// Initialize buffers
void initCircularBuffer() {
    sensorDataBuffer.reserve(samplesPerInterval);
    longTermBuffer.reserve(longTermBufferCapacity);
    loadHistoryFromNVS();
    circularBufferInitialized = true;
}

/**
 * @brief Returns buffer data as a JSON string
 * @param start The starting index of the data to retrieve
 * @param end The ending index of the data to retrieve
 * @return JSON string of buffer data
 */
String getBufferData(int start, int end) {
    uint64_t currentTime = millis();
    uint64_t intervalDuration = static_cast<uint64_t>(movingAverageInterval) * 1000ULL;
    uint64_t totalSpan = (longTermBuffer.size() > 1) ? static_cast<uint64_t>(longTermBuffer.size() - 1) * intervalDuration : 0ULL;
    uint64_t lastTimestamp = (currentTime >= totalSpan) ? (currentTime - totalSpan) : 0ULL;

    String data = "{";
    data += "\"start\":" + String(start) + ",";
    data += "\"end\":" + String(end) + ",";
    data += "\"lastTimestamp\":" + String(lastTimestamp) + ",";
    data += "\"intervalDuration\":" + String(intervalDuration) + ",";
    data += "\"data\":[";

    for (int i = start; i <= end; i++) {
        if (i > start) {
            data += ",";
        }
        data += String(longTermBuffer[i]);
    }

    data += "]}";
    return data;
}

/**
 * @brief Returns the current buffer configuration as a JSON string
 * @return JSON string of buffer configuration
 */
String getBufferConfig() {
    String config = "{";
    config += "\"sensorSampleInterval\":" + String(sensorSampleInterval) + ",";
    config += "\"movingAverageInterval\":" + String(movingAverageInterval) + ",";
    config += "\"longTermBufferCapacity\":" + String(longTermBufferCapacity);
    config += "}";
    return config;
}

/**
 * @brief Updates the buffer configuration
 * @param sensorSampleInterval New sample interval in seconds
 * @param movingAverageInterval New moving average interval in seconds
 * @param longTermBufferCapacity New capacity of the long-term buffer
 * @return JSON string of updated buffer configuration
 */
String updateBufferConfig(uint16_t newSensorSampleInterval, uint16_t newMovingAverageInterval, uint16_t newLongTermBufferCapacity) {
    bool capacityChanged = (newLongTermBufferCapacity != longTermBufferCapacity);
    sensorSampleInterval = newSensorSampleInterval;
    movingAverageInterval = newMovingAverageInterval;
    longTermBufferCapacity = newLongTermBufferCapacity;
    samplesPerInterval = movingAverageInterval / sensorSampleInterval;
    if (capacityChanged && circularBufferInitialized) {
        longTermBuffer.reserve(longTermBufferCapacity);
        syncFullHistoryToNVS();
    }
    return getBufferConfig();
}

#endif  // CO2_Gadget_CircularBufferManager_h
