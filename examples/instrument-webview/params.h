// The parameters' CLAP ids, for plugin.cpp (its parameter table lists them
// in this order). The page, in JavaScript, finds each by its name instead.
// A new parameter: an id here, before P_COUNT, and a row in that table.
#pragma once

enum ParamId
{
    P_CUTOFF,  // Hz
    P_RELEASE, // seconds
    P_COUNT    // how many
};
