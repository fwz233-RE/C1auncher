//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2013-10-17 GONG Chen <chen.sst@gmail.com>
//

#include <rime_api.h>
#include <rime/common.h>
#include <rime/registry.h>

// built-in components
#include <rime/config.h>
#include <rime/config/plugins.h>
#include <rime/schema.h>

using namespace rime;

static void rime_core_initialize() {
  LOG(INFO) << "registering core components.";
  Registry& r = Registry::instance();

  auto config_builder =
      new ConfigComponent<ConfigBuilder>([&](ConfigBuilder* builder) {
        builder->InstallPlugin(new AutoPatchConfigPlugin);
        builder->InstallPlugin(new DefaultConfigPlugin);
        builder->InstallPlugin(new LegacyPresetConfigPlugin);
        builder->InstallPlugin(new LegacyDictionaryConfigPlugin);
        builder->InstallPlugin(new BuildInfoPlugin);
        builder->InstallPlugin(new SaveOutputPlugin);
      });
  r.Register("config_builder", config_builder);

  auto config_loader =
      new ConfigComponent<ConfigLoader, DeployedConfigResourceProvider>;
  r.Register("config", config_loader);
  // term-ime patch: SchemaComponent uses config_builder (not config_loader) so
  // __include/__patch in schema files are resolved at load time. config_loader
  // only reads pre-compiled staging files (which don't exist on a fresh deploy
  // from source .yaml), leaving schemas incomplete and process_key() returns 0.
  // config_builder resolves includes AND SaveOutputPlugin writes the compiled
  // schema to staging for subsequent loads.
  r.Register("schema", new SchemaComponent(config_builder));

  auto user_config =
      new ConfigComponent<ConfigLoader, UserConfigResourceProvider>(
          [](ConfigLoader* loader) { loader->set_auto_save(true); });
  r.Register("user_config", user_config);
}

static void rime_core_finalize() {
  // registered components have been automatically destroyed prior to this call
}

RIME_REGISTER_MODULE(core)
