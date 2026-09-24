#pragma once
#include <clap/clap.h>

// What entry.cpp needs from the plugin: each example's plugin.cpp defines these.
const clap_plugin_descriptor_t *getPluginDescriptor();
const clap_plugin_t *createPlugin(const clap_host_t *host);
