#include "App.hpp"
#if !RVE_SERVER
    #include "RenderEngine.hpp"
    #include <SDL_events.h>
    #include "Texture.hpp"
    #include <RmlUi/Core.h>
    #include "GUI.hpp"
    #include "Material.hpp"
    #include "RMLFileInterface.hpp"
    #include "InputManager.hpp"
    #include "Skybox.hpp"
    #include <SDL.h>
    #include "AudioPlayer.hpp"
    #include "Window.hpp"
    #include <RGL/Swapchain.hpp>
    #include <RGL/CommandBuffer.hpp>
    #include "OpenXRIntegration.hpp"
#endif
#include <algorithm>
#include "MeshAsset.hpp"
#include <physfs.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <filesystem>
#include "Function.hpp"
#include "World.hpp"
#include "GetApp.hpp"
#include "Defines.hpp"
#include "VirtualFileSystem.hpp"
#include "MeshAssetSkinned.hpp"

#include "CameraComponent.hpp"
#include <csignal>
#include "Debug.hpp"

#ifdef _WIN32
	#include <Windows.h>
	#include <winuser.h>
	#undef min
#endif

#ifdef __APPLE__
    #include "AppleUtilities.h"
#endif

using namespace std;
using namespace RavEngine;
using namespace std::chrono;

// pointer to the current app instance
static App* currentApp = nullptr;

// on crash, call this
void crash_signal_handler(int signum) {
	::signal(signum, SIG_DFL);
	Debug::PrintStacktraceHere();
	::raise(SIGABRT);
}

/**
 GameNetworkingSockets debug log function
 */
static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
{
	if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Bug )
	{
		Debug::Fatal("{}",pszMsg);
	}
	else{
		Debug::Log("{}",pszMsg);
	}
}

App::App()
#if !RVE_SERVER
: player(std::make_unique<AudioPlayer>())
#endif
{
    currentApp = this;
	// crash signal handlers
	::signal(SIGSEGV, &crash_signal_handler);
	::signal(SIGABRT, &crash_signal_handler);

	//initialize virtual file system library 
	PHYSFS_init("");
#if !RVE_SERVER
	Resources = std::make_unique<VirtualFilesystem>();
#endif
}

int App::run(int argc, char** argv) {
#if !RVE_SERVER
	// initialize SDL2
	if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | SDL_INIT_HAPTIC | SDL_INIT_VIDEO) != 0) {
		Debug::Fatal("Unable to initialize SDL2: {}", SDL_GetError());
	}
	{
		auto config = OnConfigure(argc, argv);

		// initialize RGL and the global Device
		RGL::API api = RGL::API::PlatformDefault;
		{
#if _UWP
			size_t n_elt;
			char* envv;
			_dupenv_s(&envv, &n_elt, "RGL_BACKEND");
#else
			auto envv = std::getenv("RGL_BACKEND");
#endif
			if (envv == nullptr) {
				goto cont;
			}
			auto backend = std::string_view(envv);

			const std::unordered_map<std::string_view, RGL::API> apis{
				{"metal", decltype(apis)::value_type::second_type::Metal},
				{ "d3d12", decltype(apis)::value_type::second_type::Direct3D12 },
				{ "vulkan", decltype(apis)::value_type::second_type::Vulkan },
			};

			auto it = apis.find(backend);
			if (it != apis.end()) {
				api = (*it).second;
			}
			else {
				std::cerr << "No backend \"" << backend << "\", expected one of:\n";
				for (const auto& api : apis) {
					std::cout << "\t - " << RGL::APIToString(api.second) << "\n";
				}
			}
		}
	cont:

		RGL::InitOptions opt{
			.api = api,
			.engineName = "RavEngine",
		};
		RGL::Init(opt);

		device = RGL::IDevice::CreateSystemDefaultDevice();

		Renderer = std::make_unique<RenderEngine>(config, device);

		window = std::make_unique<Window>(960, 540, "RavEngine", device, Renderer->mainCommandQueue);
        auto size = window->GetSizeInPixels();
        mainWindowView = { Renderer->CreateRenderTargetCollection({ static_cast<unsigned int>(size.width), static_cast<unsigned int>(size.height) })};

#ifdef RVE_XR_AVAILABLE
		if (wantsXR) {
			OpenXRIntegration::init_openxr({
				.device = device,
				.commandQueue = Renderer->mainCommandQueue
			});
			xrRenderViewCollections = OpenXRIntegration::CreateRenderTargetCollections();
		}
#endif
	}
	
	//setup GUI rendering
	Rml::SetSystemInterface(&GetRenderEngine());
	Rml::SetRenderInterface(&GetRenderEngine());
	Rml::SetFileInterface(new VFSInterface());
	Rml::Initialise();

#ifndef NDEBUG
	Renderer->InitDebugger();
#endif

#ifdef __APPLE__
	enableSmoothScrolling();
#endif

	//load the built-in fonts
	App::Resources->IterateDirectory("fonts", [](const std::string& filename) {
		auto p = Filesystem::Path(filename);
		if (p.extension() == ".ttf") {
			GUIComponent::LoadFont(p.filename().string());
		}
		});

	//setup Audio
	player->Init();
#endif
	//setup networking
	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
		Debug::Fatal("Networking initialization failed: {}", errMsg);
	}
	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
	
	// if built in non-UWP for Windows, need to manually set DPI awareness
	// for some weird reason, it's not present on ARM
#if defined _WIN32 && !_WINRT && !defined(_M_ARM64)
	SetProcessDPIAware();
	//SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
#if !RVE_SERVER

	{
		//make the default texture white
		uint8_t data[] = {0xFF,0xFF,0xFF,0xFF};
		Texture::Manager::defaultTexture = make_shared<RuntimeTexture>(1, 1, Texture::Config{
			.mipLevels = 1,
			.numLayers = 1,
			.initialData = data 
			});
		
		uint8_t normalData[] = {256/2,256/2, 0xFF,0xFF};
		Texture::Manager::defaultNormalTexture = New<RuntimeTexture>(1,1, Texture::Config{
			.mipLevels = 1,
			.numLayers = 1,
			.initialData = normalData
		});
	}
#endif

	//invoke startup hook
	OnStartup(argc, argv);
	
	lastFrameTime = clocktype::now();
   
#if !RVE_SERVER
	bool exit = false;
	SDL_Event event;
	while (!exit) {
#if __APPLE__
		@autoreleasepool{
#endif

			//setup framerate scaling for next frame
			auto now = clocktype::now();
		//will cause engine to run in slow motion if the frame rate is <= 1fps
		deltaTimeMicroseconds = std::min(duration_cast<timeDiff>(now - lastFrameTime), maxTimeStep);
		float deltaSeconds = std::chrono::duration<decltype(deltaSeconds)>(deltaTimeMicroseconds).count();
		time += deltaSeconds;
		currentScale = deltaSeconds * evalNormal;

		auto windowflags = SDL_GetWindowFlags(window->window);
		while (SDL_PollEvent(&event)) {
			switch (event.type) {
				case SDL_QUIT:
					exit = true;
					break;

				case SDL_WINDOWEVENT: {
					const SDL_WindowEvent& wev = event.window;
					switch (wev.event) {
						case SDL_WINDOWEVENT_RESIZED:
						case SDL_WINDOWEVENT_SIZE_CHANGED:
							Renderer->mainCommandQueue->WaitUntilCompleted();
							window->NotifySizeChanged(wev.data1, wev.data2);
                            {
								auto size = window->GetSizeInPixels();
                                Renderer->ResizeRenderTargetCollection(mainWindowView.collection, {uint32_t(size.width), uint32_t(size.height)});
                            }
							break;

						case SDL_WINDOWEVENT_CLOSE:
							exit = true;
							break;
					}
				} break;
			}
			//process others
			if (inputManager) {
				inputManager->ProcessInput(event,windowflags,currentScale, window->windowdims.width, window->windowdims.height);
#ifndef NDEBUG
				RenderEngine::debuggerInput->ProcessInput(event,windowflags,currentScale, window->windowdims.width, window->windowdims.height);
#endif
			}
		}

#ifndef NDEBUG
		RenderEngine::debuggerInput->TickAxes();
#endif
		if (inputManager) {
			inputManager->TickAxes();
		}

		auto windowSize = window->GetSizeInPixels();
		auto scale = window->GetDPIScale();
#endif
		//tick all worlds
		for (const auto world : loadedWorlds) {
			world->Tick(currentScale);
#if !RVE_SERVER
			world->Filter([=](GUIComponent& gui) {
				if (gui.Mode == GUIComponent::RenderMode::Screenspace) {
					gui.SetDimensions(windowSize.width, windowSize.height);
					gui.SetDPIScale(scale);
				}
				gui.Update();
			});
#endif
		}

		//process main thread tasks
		{
			Function<void(void)> front;
			while (main_tasks.try_dequeue(front)) {
				front();
			}
		}
#if !RVE_SERVER
		auto nextTexture = window->GetNextSwapchainImage();
		mainWindowView.collection.finalFramebuffer = nextTexture.texture;

		// get the cameras to render
		auto allCameras = renderWorld->GetAllComponentsOfType<CameraComponent>();

		if (!allCameras)
		{
			Debug::Fatal("Cannot render: World does not have a camera!");
		}
		mainWindowView.camDatas.clear();
		for (const auto& camera : *allCameras) {
			if (!camera.IsActive()) {
				continue;
			}
			auto viewProj = camera.GenerateProjectionMatrix(windowSize.width, windowSize.height) * camera.GenerateViewMatrix();
			auto camPos = camera.GetOwner().GetTransform().GetWorldPosition();
			auto viewportOverride = camera.viewportOverride;
            mainWindowView.camDatas.push_back(RenderViewCollection::camData{viewProj, camPos, viewportOverride});
		}

		mainWindowView.pixelDimensions = window->GetSizeInPixels();

		std::vector<RenderViewCollection> allViews;
		allViews.push_back(mainWindowView);

#ifdef RVE_XR_AVAILABLE
		// update OpenXR data if it is requested
		std::pair<std::vector<XrView>, XrFrameState> xrBeginData;
		if (wantsXR) {
			xrBeginData = OpenXRIntegration::BeginXRFrame();
			OpenXRIntegration::UpdateXRTargetCollections(xrRenderViewCollections, xrBeginData.first);
			allViews.insert(allViews.end(), xrRenderViewCollections.begin(), xrRenderViewCollections.end());
		}
#endif

		auto mainCommandBuffer = Renderer->Draw(renderWorld, allViews,scale);

		// show the results to the user
		RGL::CommitConfig commitconfig{
			.signalFence = window->swapchainFence,
		};
		mainCommandBuffer->Commit(commitconfig);
		mainCommandBuffer->BlockUntilCompleted();

		window->swapchain->Present(nextTexture.presentConfig);

#ifdef RVE_XR_AVAILABLE
		if (wantsXR) {
			OpenXRIntegration::EndXRFrame(xrBeginData.second);
		}
#endif

		player->SetWorld(renderWorld);
		//make up the difference
		//can't use sleep because sleep is not very accurate
		/*clocktype::duration work_time;
		do{
			auto workEnd = clocktype::now();
			work_time = workEnd - now;
			auto delta = min_tick_time - work_time;
			if (delta > std::chrono::duration<double, std::milli>(3)) {
				auto dc = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
				std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(dc.count()-1));
			}
		}while (work_time < min_tick_time);*/
	skip_xr_frame:

		lastFrameTime = now;
#if __APPLE__
		}	// end of @autoreleasepool
#endif
	}
#endif  // !RVE_SERVER
	
    return OnShutdown();
}

float App::CurrentTPS() {
	return App::evalNormal / currentScale;
}

/**
Set the current world to tick automatically
@param newWorld the new world
*/

void RavEngine::App::SetRenderedWorld(Ref<World> newWorld) {
	if (!loadedWorlds.contains(newWorld)) {
		Debug::Fatal("Cannot render an inactive world");
	}
	if (renderWorld) {
		renderWorld->OnDeactivate();
		renderWorld->isRendering = false;
	}
	renderWorld = newWorld;
	renderWorld->isRendering = true;
	renderWorld->OnActivate();
}

/**
Add a world to be ticked
@param world the world to tick
*/

void RavEngine::App::AddWorld(Ref<World> world) {
	loadedWorlds.insert(world);
	if (!renderWorld) {
		SetRenderedWorld(world);
	}

	// synchronize network if necessary
	if (networkManager.IsClient() && !networkManager.IsServer()) {
		networkManager.client->SendSyncWorldRequest(world);
	}
}

/**
Remove a world from the tick list
@param world the world to tick
*/

void RavEngine::App::RemoveWorld(Ref<World> world) {
	loadedWorlds.erase(world);
	if (renderWorld == world) {
		renderWorld->OnDeactivate();
		renderWorld.reset();    //this will cause nothing to render, so set a different world as rendered
	}
}

/**
* Unload all worlds
*/

void RavEngine::App::RemoveAllWorlds() {
	for (const auto& world : loadedWorlds) {
		RemoveWorld(world);
	}
}

/**
Replace a loaded world with a different world, transferring render state if necessary
@param oldWorld the world to replace
@param newWorld the world to replace with. Cannot be already loaded.
*/

void RavEngine::App::AddReplaceWorld(Ref<World> oldWorld, Ref<World> newWorld) {
	AddWorld(newWorld);
	bool updateRender = renderWorld == oldWorld;
	RemoveWorld(oldWorld);
	if (updateRender) {
		SetRenderedWorld(newWorld);
	}
}

void App::Quit(){
#if !RVE_SERVER
	SDL_Event event;
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);
#else
    Debug::Fatal("Quit is not implemented on the server (TODO)");
#endif
}

App::~App(){
    if (!PHYSFS_isInit()){  // unit tests do not initialize the vfs, so we don't want to procede here
        return;
    }
#if !RVE_SERVER
	inputManager = nullptr;
#endif
	renderWorld = nullptr;
	loadedWorlds.clear();
#if !RVE_SERVER
#ifndef NDEBUG
	Renderer->DeactivateDebugger();
#endif
#endif
    MeshAsset::Manager::Clear();
    MeshAssetSkinned::Manager::Clear();
    
#if !RVE_SERVER

	Texture::Manager::defaultTexture.reset();
    Texture::Manager::Clear();
	player->Shutdown();
#endif
	networkManager.server.reset();
	networkManager.client.reset();
	GameNetworkingSockets_Kill();
	PHYSFS_deinit();
#if !RVE_SERVER

	auto fsi = Rml::GetFileInterface();
	Rml::Shutdown();
    Renderer.reset();
    delete fsi;
#endif

	currentApp = nullptr;
}

#if !RVE_SERVER
void App::SetWindowTitle(const char *title){
	SDL_SetWindowTitle(window->window, title);
}
#endif

std::optional<Ref<World>> RavEngine::App::GetWorldByName(const std::string& name) {
	std::optional<Ref<World>> value;
	for (const auto& world : loadedWorlds) {
		// because std::string "world\0\0" != "world", we need to use strncmp
		if (std::strncmp(world->worldID.data(), name.data(), World::id_size) == 0) {
			value.emplace(world);
			break;
		}
	}
	return value;
}

void App::OnDropAudioWorklets(uint32_t nDropped){
    Debug::Warning("Dropped {} audio tasks.", nDropped);
}


App* RavEngine::GetApp()
{
	return currentApp;
}
