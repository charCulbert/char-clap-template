// The parameters' CLAP ids and how their values read as text, for plugin.cpp
// (its parameter table lists the ids in this order) and for the interface
// (ui/PluginView.cpp).
// A new parameter: an id here, before P_COUNT, and a row in that table.
#pragma once
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum ParamId
{
    P_CUTOFF,  // Hz
    P_RELEASE, // seconds
    P_WAVE,    // 0 saw, 1 square, 2 triangle: stepped
    P_COUNT    // how many
};

// How each unit's values read as text: for the host, through plugin.cpp's
// value_to_text and text_to_value (a parameter's unit is in its row there),
// and for the interface, which shows and reads values the same way.
// A new unit: a name here, and a case in both functions.
enum ParamUnit
{
    UNIT_HERTZ,   // "440 Hz", "1.20 kHz"
    UNIT_SECONDS, // "300 ms", "1.20 s"
    UNIT_WAVE,    // "Saw", "Square", "Triangle"
};

inline const char* const waveNames[] = { "Saw", "Square", "Triangle" };

inline bool ParamValueToText (ParamUnit unit, double value, char* display, uint32_t size)
{
    switch (unit)
    {
        case UNIT_HERTZ:
            snprintf (display, size, value < 1000 ? "%.0f Hz" : "%.2f kHz", value < 1000 ? value : value / 1000);
            return true;
        case UNIT_SECONDS:
            snprintf (display, size, value < 1 ? "%.0f ms" : "%.2f s", value < 1 ? value * 1000 : value);
            return true;
        case UNIT_WAVE: // stepped: CLAP truncates
            snprintf (display, size, "%s", waveNames[int (fmin (fmax (value, 0.0), 2.0))]);
            return true;
    }
    return false;
}

// What a typed number counts in, as the value times this: a bare number, in
// the smaller unit shown ("300" is 300 ms; "2k" and "1.2 s" aren't numbers).
// The interface types values in it too.
inline double ParamTypedScale (ParamUnit unit)
{
    return unit == UNIT_SECONDS ? 1000 : 1; // ms; Hz; a waveform's number
}

// A number at the start of text, with '.' or ',' as its decimal point
// whatever locale the host set (strtod alone reads only the locale's).
inline double ParamReadNumber (const char* text, char** end)
{
    char copy[64];
    snprintf (copy, sizeof (copy), "%s", text);
    for (char* c = copy; *c; c++)
        if (*c == '.' || *c == ',')
            *c = *localeconv()->decimal_point;
    char* stop = nullptr;
    const double number = strtod (copy, &stop);
    *end = (char*) text + (stop - copy);
    return number;
}

inline bool ParamTextToValue (ParamUnit unit, const char* display, double* value)
{
    char* end = nullptr;
    const double number = ParamReadNumber (display, &end); // "440 Hz" reads as 440
    const bool read = end != display;
    while (*end == ' ')
        end++;
    if (read && !*end) // a bare number, as typed
        return *value = number / ParamTypedScale (unit), true;
    switch (unit) // else the text value_to_text wrote, as a host may read it back
    {
        case UNIT_HERTZ: // "1.20 kHz", "440 Hz"
            *value = strcmp (end, "kHz") == 0 ? number * 1000 : number;
            return read && (strcmp (end, "kHz") == 0 || strcmp (end, "Hz") == 0);
        case UNIT_SECONDS: // "300 ms", "1.20 s"
            *value = strcmp (end, "ms") == 0 ? number / 1000 : number;
            return read && (strcmp (end, "ms") == 0 || strcmp (end, "s") == 0);
        case UNIT_WAVE: // "Square"
            for (int i = 0; i < 3; i++)
                if (strcmp (display, waveNames[i]) == 0)
                    return *value = i, true;
            return false;
    }
    return false;
}
