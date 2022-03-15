//
// CollabVM Server
//
// (C) 2021-2022 CollabVM Development Team
//
// This file is licensed under the GNU General Public License Version 3.
// Text is provided in LICENSE.
//

#include <core/PluginManager.h>
#include <boost/dll.hpp>

// TODO: Should find a better place to include this
#define SPDLOG_FMT_EXTERNAL // why do I need to define this lol
#include <spdlog/spdlog.h>

namespace collabvm::core {
		// While I could force this into PluginManager there's
		// expected to only be one implementation of the APIs here

		static std::vector<boost::dll::shared_library> g_PluginSos;

		static std::vector<plugin::IServerPlugin*> g_ServerPlugins;
		//static std::vector<plugin::ICorePlugin*> g_CorePlugins;
		//static std::vector<plugin::IControllerPlugin*> g_ControllerPlugins;

		struct PluginApiImpl : public plugin::IPluginApi {

			void WriteLogMessageImpl(plugin::IPluginApi::LogLevel logLevel, const collabvm::plugin::utf8char* message, ...) {
				char buf[2048]; // I hope this is big enough lol
				va_list va;
				auto mp = reinterpret_cast<const char*>(message);

				va_start(va, message);
				vsprintf(&buf[0], mp, va);
				va_end(va);

				// hope your compiler supports this!
				using enum plugin::IPluginApi::LogLevel;
				switch(logLevel) {
					case Info:
						spdlog::info(std::string_view(buf));
						break;
					case Warning:
						spdlog::warn(buf);
						break;
					case Error:
						spdlog::error(buf);
						break;
				}
			}

			// TODO: We'll probably wanna maintain a separate heap, instead of just lazily
			// handing everything off to malloc.

			void* MallocImpl(std::size_t c) {
				return malloc(c);
			}

			void FreeImpl(void* p) {
				free(p);
			}

			PluginApiImpl() : IPluginApi() {
				COLLABVM_PLUGINABI_ASSIGN_VTFUNC(WriteLogMessage, &PluginApiImpl::WriteLogMessageImpl);
				COLLABVM_PLUGINABI_ASSIGN_VTFUNC(Malloc, &PluginApiImpl::MallocImpl);
				COLLABVM_PLUGINABI_ASSIGN_VTFUNC(Free, &PluginApiImpl::FreeImpl);
			}

		};

		/**
		 * Global instance of the implementation of the plugin API,
		 * consumed by:
		 * - Coreplugins
		 * - Server plugins
		 * - ControllerPlugins
		 */
		static PluginApiImpl g_PluginApiImpl;

		enum class PluginLoadError {
			NoError,
			NotPlugin,
			AbiMismatch,
			ExportNotFound,
			NotServerPlugin,
			NotCorePlugin,
		};

		/**
		 * Handy template function to "handshake" with plugins
		 */
		template<bool CorePluginHandShake = false, bool ControllerPluginHandShake = false>
		static PluginLoadError PluginHandshake(boost::dll::shared_library& so) {
			// verify ABI version. This is only needed once
			// so we put it in scope to knock it off once we're done.
			{
				auto collabvm_plugin_abi_version = so.get<int()>("collabvm_plugin_abi_version");

				if(!collabvm_plugin_abi_version)
					return PluginLoadError::NotPlugin; // no ABI symbol?

				// big problem.
				if(collabvm_plugin_abi_version() != plugin::PLUGIN_ABI_VERSION)
					return PluginLoadError::AbiMismatch; // invalid ABI
			}

			// now we can do the fun stuff!
			auto collabvm_plugin_init_api = so.get<void(collabvm::plugin::IPluginApi*)>("collabvm_plugin_init_api");

			if(!collabvm_plugin_init_api)
				return PluginLoadError::ExportNotFound;

			// feed it plugin API
			collabvm_plugin_init_api(&g_PluginApiImpl);

			if constexpr(!CorePluginHandShake && !ControllerPluginHandShake) {
				auto collabvm_plugin_make_serverplugin = so.get<collabvm::plugin::IServerPlugin*()>("collabvm_plugin_make_serverplugin");
				auto collabvm_plugin_delete_serverplugin = so.get<void(collabvm::plugin::IServerPlugin*)>("collabvm_plugin_delete_serverplugin");

				if(!collabvm_plugin_make_serverplugin || !collabvm_plugin_delete_serverplugin)
					return PluginLoadError::ExportNotFound;

				auto plugin = collabvm_plugin_make_serverplugin();

				if(!plugin)
					return PluginLoadError::NotServerPlugin; // we have other issues

				g_ServerPlugins.push_back(plugin);
			} else if constexpr(CorePluginHandShake) {
				// coreplugin handshake: TODO
			} else if constexpr (!CorePluginHandShake && ControllerPluginHandShake) {
				// controllerplugin handshake: TODO
				// (we will need to treat it a bit differently,
				// as its method is a factory)
			}

			return PluginLoadError::NoError;
		}

		template<bool coreplugin = false, bool controllerplugin = false>
		static PluginLoadError HandlePluginLoad(boost::dll::shared_library& so) {
			auto res = PluginHandshake<coreplugin, controllerplugin>(so);

			if(res != PluginLoadError::NoError) {
				switch(res) {
					case PluginLoadError::NotServerPlugin: {
						// Try loading as a coreplugin next
						return HandlePluginLoad<true, false>(so);
					}; break;

					case PluginLoadError::NotCorePlugin: {
						// Try loading as a controllerplugin.
						return HandlePluginLoad<false, true>(so);
					}; break;
				}

				return res;
			}
		}

		bool PluginManager::Init() {
			// resize to a sane size
			g_PluginSos.resize(5);
			
			if(!std::filesystem::is_directory(std::filesystem::current_path() / "plugins") || !std::filesystem::exists(std::filesystem::current_path() / "plugins")) {
				// TODO: Throw error if we don't have write permissions
				spdlog::info("PluginManager::Init: Plugins folder not found. Creating folder.");
				std::filesystem::create_directory(std::filesystem::current_path() / "plugins");
			}

			for(auto& it : std::filesystem::directory_iterator(std::filesystem::current_path() / "plugins")) {
				spdlog::info("Going to load plugin {}", it.path().string());
				if(!this->LoadPlugin(it.path())) {
					spdlog::warn("Plugin {} failed to load :(", it.path().string());
				}
			}

			// FIXME: would there ever be a fatal case?
			return true;
		}

		void PluginManager::UnloadPlugins() {
			// TODO: implement me
			spdlog::info("PluginManager::UnloadPlugins: TODO");
		}


		bool PluginManager::LoadPlugin(const std::filesystem::path& path) {
			g_PluginSos.emplace_back(boost::dll::shared_library(path.string()));

			auto res = HandlePluginLoad<>(g_PluginSos.back());
			switch(res) {
				case PluginLoadError::ExportNotFound:
					spdlog::error("plugin {} is probably NOT a collabvm server plugin", path.string());
					return false;
					break;

				// FIXME: What ABI version is it?
				case PluginLoadError::AbiMismatch:
					spdlog::error("plugin {} has an mismatching CollabVM ABI version.", path.string());
					return false;
					break;
			}

			// otherwise, we're probably fine
			return true;
		}

}
