// See the file "COPYING" in the main distribution directory for copyright.

#ifndef BRO_PLUGIN_ENDACE_DAG
#define BRO_PLUGIN_ENDACE_DAG

#include <plugin/Plugin.h>

namespace plugin {
namespace Endace_DAG {

class Plugin : public ::plugin::Plugin
{
protected:
	// Overridden from plugin::Plugin.
	plugin::Configuration Configure() override;
};

extern Plugin plugin;

}
}

#endif
