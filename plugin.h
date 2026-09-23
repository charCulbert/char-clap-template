#pragma once
#include <clap/clap.h>

const clap_plugin_descriptor_t *getPluginDescriptor();
const clap_plugin_t *createPlugin(const clap_host_t *host);