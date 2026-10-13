// The parameters' CLAP ids, for plugin.cpp (its parameter table lists them
// in this order) and for an interface, if you add one.
// A new parameter: an id here, before P_COUNT, and a row in that table.
#pragma once

enum ParamId
{
    P_GAIN, // dB
    P_PAN,  // -1 left .. 1 right
    P_COUNT // how many
};
