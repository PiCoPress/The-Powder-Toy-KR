#include "PowderToySDL.h"
#include "Format.h"
#include "X86KillDenormals.h"
#include "prefs/GlobalPrefs.h"
#include "client/Client.h"
#include "client/GameSave.h"
#include "client/SaveFile.h"
#include "client/SaveInfo.h"
#include "client/http/requestmanager/RequestManager.h"
#include "client/http/GetSaveRequest.h"
#include "client/http/GetSaveDataRequest.h"
#include "common/platform/Platform.h"
#include "graphics/Graphics.h"
#include "simulation/SaveRenderer.h"
#include "simulation/SimulationData.h"
#include "common/tpt-rand.h"
#include "gui/game/Favorite.h"
#include "gui/Style.h"
#include "gui/game/GameController.h"
#include "gui/game/GameView.h"
#include "gui/game/IntroText.h"
#include "gui/dialogues/ConfirmPrompt.h"
#include "gui/dialogues/ErrorMessage.h"
#include "gui/interface/Engine.h"
#include "gui/interface/TextWrapper.h"
#include "Config.h"
#include "SimulationConfig.h"
#include <optional>
#include <climits>
#include <iostream>
#include <csignal>
#include <SDL.h>
#include <exception>
#include <cstdlib>

void LoadWindowPosition()
{
	if (Client::Ref().IsFirstRun())
	{
		return;
	}

	auto &prefs = GlobalPrefs::Ref();
	int savedWindowX = prefs.Get("WindowX", INT_MAX);
	int savedWindowY = prefs.Get("WindowY", INT_MAX);

	int borderTop, borderLeft;
	SDL_GetWindowBordersSize(sdl_window, &borderTop, &borderLeft, nullptr, nullptr);
	// Sometimes (Windows), the border size may not be reported for 200+ frames
	// So just have a default of 5 to ensure the window doesn't get stuck where it can't be moved
	if (borderTop == 0)
		borderTop = 5;

	int numDisplays = SDL_GetNumVideoDisplays();
	SDL_Rect displayBounds;
	bool ok = false;
	for (int i = 0; i < numDisplays; i++)
	{
		SDL_GetDisplayBounds(i, &displayBounds);
		if (savedWindowX + borderTop > displayBounds.x && savedWindowY + borderLeft > displayBounds.y &&
				savedWindowX + borderTop < displayBounds.x + displayBounds.w &&
				savedWindowY + borderLeft < displayBounds.y + displayBounds.h)
		{
			ok = true;
			break;
		}
	}
	if (ok)
		SDL_SetWindowPosition(sdl_window, savedWindowX + borderLeft, savedWindowY + borderTop);
}

void SaveWindowPosition()
{
	int x, y;
	SDL_GetWindowPosition(sdl_window, &x, &y);

	int borderTop, borderLeft;
	SDL_GetWindowBordersSize(sdl_window, &borderTop, &borderLeft, nullptr, nullptr);

	auto &prefs = GlobalPrefs::Ref();
	prefs.Set("WindowX", x - borderLeft);
	prefs.Set("WindowY", y - borderTop);
}

void LargeScreenDialog()
{
	StringBuilder message;
	auto scale = ui::Engine::Ref().windowFrameOps.scale;
	message << "큰 화면에 맞춰 " << scale << "x 크기로 전환합니다: ";
	message << desktopWidth << "x" << desktopHeight << " 감지됨, " << WINDOWW * scale << "x" << WINDOWH * scale << " 필요함.";
	message << "\n취소하려면 취소 단추를 누르세요. 나중에 설정에서 언제든지 바꿀 수 있습니다.";
	new ConfirmPrompt("대화면 감지됨", message.Build(), { nullptr, []() {
		GlobalPrefs::Ref().Set("Scale", 1);
		ui::Engine::Ref().windowFrameOps.scale = 1;
	} });
}

void TickClient()
{
	Client::Ref().Tick();
}

static void BlueScreen(String detailMessage, std::optional<std::vector<String>> stackTrace)
{
	auto &engine = ui::Engine::Ref();
	engine.g->BlendFilledRect(engine.g->Size().OriginRect(), 0x1172A9_rgb .WithAlpha(0xD2));

	auto crashPrevLogPath = ByteString("crash.prev.log");
	auto crashLogPath = ByteString("crash.log");
	Platform::RenameFile(crashLogPath, crashPrevLogPath, true);

	StringBuilder crashInfo;
	crashInfo << "ERROR - Details: " << detailMessage << "\n";
	crashInfo << "복구 불가능한 오류가 발생했습니다. 공식 홈페이지에서 제보해주세요.\n\n  " << SCHEME << SERVER << "\n\n";
	crashInfo << "데이터 폴더에 있는 " << crashLogPath.FromUtf8() << " 파일의 정보가 모두 저장됩니다. \n";
	// 적절한 의미를 못찾겠어요
	crashInfo << "버그를 제보하려면 파일을 첨부해야 합니다.\n\n";
	crashInfo << "Version: " << VersionInfo().FromUtf8() << "\n";
	crashInfo << "Tag: " << VCS_TAG << "\n";
	crashInfo << "Date: " << format::UnixtimeToDate(time(NULL), "%Y-%m-%dT%H:%M:%SZ", false).FromUtf8() << "\n";
	if (stackTrace)
	{
		crashInfo << "Stack trace:\n";
		for (auto &item : *stackTrace)
		{
			crashInfo << " - " << item << "\n";
		}
	}
	else
	{
		crashInfo << "Stack trace not available\n";
	}
	String errorText = crashInfo.Build();
	constexpr auto width = 440;
	ui::TextWrapper tw;
	tw.Update(errorText, true, width);
	engine.g->BlendText(ui::Point((engine.g->Size().X - width) / 2, 80), tw.WrappedText(), 0xFFFFFF_rgb .WithAlpha(0xFF));

	auto crashLogData = errorText.ToUtf8();
	std::cerr << crashLogData << std::endl;
	Platform::WriteFile(std::vector<char>(crashLogData.begin(), crashLogData.end()), crashLogPath);

	//Death loop
	SDL_Event event;
	auto running = true;
	while (running)
	{
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_QUIT)
			{
				running = false;
			}
		}
		blit(engine.g->Data());
	}

	// Don't use Platform::Exit, we're practically zombies at this point anyway.
#if defined(__MINGW32__) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
	// Come on...
	exit(-1);
#else
	quick_exit(-1);
#endif
}

static struct
{
	int sig;
	const char *message;
} signalMessages[] = {
	{ SIGSEGV, "메모리 읽기/쓰기 오류" }, 
	{ SIGFPE, "실수값 연산 중 오류" },
	{ SIGILL, "프로그램 오류 발생" },
	{ SIGABRT, "예상치 못한 프로그램 중단" },
	{ 0, nullptr },
};

static void SigHandler(int signal)
{
	const char *message = "알 수 없는 신호";
	for (auto *msg = signalMessages; msg->message; ++msg)
	{
		if (msg->sig == signal)
		{
			message = msg->message;
			break;
		}
	}
	BlueScreen(ByteString(message).FromUtf8(), Platform::StackTrace());
}

static void TerminateHandler()
{
	ByteString err = "std::terminate called without a current exception";
	auto eptr = std::current_exception();
	try
	{
		if (eptr)
		{
			std::rethrow_exception(eptr);
		}
	}
	catch (const std::exception &e)
	{
		err = "처리할 수 없는 오류: " + ByteString(e.what());
	}
	catch (...)
	{
		err = "알 수 없는 문제가 발생했습니다."; // cannot determine reason of the error
	}
	BlueScreen(err.FromUtf8(), Platform::StackTrace());
}

constexpr int SCALE_MAXIMUM = 10;
constexpr int SCALE_MARGIN = 30;

int GuessBestScale()
{
	const int widthNoMargin = desktopWidth - SCALE_MARGIN;
	const int widthGuess = widthNoMargin / WINDOWW;

	const int heightNoMargin = desktopHeight - SCALE_MARGIN;
	const int heightGuess = heightNoMargin / WINDOWH;

	int guess = std::min(widthGuess, heightGuess);
	if(guess < 1 || guess > SCALE_MAXIMUM)
		guess = 1;

	return guess;
}

struct ExplicitSingletons
{
	// These need to be listed in the order they are populated in main.
	std::unique_ptr<GlobalPrefs> globalPrefs;
	http::RequestManagerPtr requestManager;
	std::unique_ptr<Client> client;
	std::unique_ptr<SaveRenderer> saveRenderer;
	std::unique_ptr<Favorite> favorite;
	std::unique_ptr<ui::Engine> engine;
	std::unique_ptr<SimulationData> simulationData;
	std::unique_ptr<GameController> gameController;
};
static std::unique_ptr<ExplicitSingletons> explicitSingletons;

int main(int argc, char *argv[])
{
	Platform::SetupCrt();
	return Platform::InvokeMain(argc, argv);
}

int Main(int argc, char *argv[])
{
	Platform::Atexit([]() {
		SaveWindowPosition();
		// Unregister dodgy error handlers so they don't try to show the blue screen when the window is closed
		for (auto *msg = signalMessages; msg->message; ++msg)
		{
			signal(msg->sig, SIG_DFL);
		}
		SDLClose();
		explicitSingletons.reset();
	});
	explicitSingletons = std::make_unique<ExplicitSingletons>();


	// https://bugzilla.libsdl.org/show_bug.cgi?id=3796
	if (SDL_Init(0) < 0)
	{
		fprintf(stderr, "SDL 준비중: %s\n", SDL_GetError());
		return 1;
	}

	Platform::originalCwd = Platform::GetCwd();

	using Argument = std::optional<ByteString>;
	std::map<ByteString, Argument> arguments;

	for (auto i = 1; i < argc; ++i)
	{
		auto str = ByteString(argv[i]);
		if (str.BeginsWith("file://"))
		{
			arguments.insert({ "open", format::URLDecode(str.substr(7 /* length of the "file://" prefix */)) });
		}
		else if (str.BeginsWith("ptsave:"))
		{
			arguments.insert({ "ptsave", str });
		}
		else if (auto split = str.SplitBy(':'))
		{
			arguments.insert({ split.Before(), split.After() });
		}
		else if (auto split = str.SplitBy('='))
		{
			arguments.insert({ split.Before(), split.After() });
		}
		else if (str == "open" || str == "ptsave" || str == "ddir")
		{
			if (i + 1 < argc)
			{
				arguments.insert({ str, argv[i + 1] });
				i += 1;
			}
			else
			{
				std::cerr << "명령줄 인자의 값이 필요합니다: " << str << std::endl;
			}
		}
		else
		{
			arguments.insert({ str, "" }); // so .has_value() is true
		}
	}

	auto ddirArg = arguments["ddir"];
	if (ddirArg.has_value())
	{
		if (Platform::ChangeDir(ddirArg.value()))
			Platform::sharedCwd = Platform::GetCwd();
		else
			perror("내부적인 오류가 발생했습니다."); // failed to chdir to requested ddir
	}
	else
	{
		auto ddir = Platform::DefaultDdir();
		if (!Platform::FileExists("powder.pref"))
		{
			if (ddir.size())
			{
				if (!Platform::ChangeDir(ddir))
				{
					perror("내부적인 오류가 발생했습니다."); // faild to chdir to default ddir
					ddir = {};
				}
			}
		}

		if (ddir.size())
		{
			Platform::sharedCwd = ddir;
		}
	}
	// We're now in the correct directory, time to get prefs.
	explicitSingletons->globalPrefs = std::make_unique<GlobalPrefs>();

	auto &prefs = GlobalPrefs::Ref();

	WindowFrameOps windowFrameOps{
		prefs.Get("Scale", 1),
		prefs.Get("Resizable", false),
		prefs.Get("Fullscreen", false),
		prefs.Get("AltFullscreen", false),
		prefs.Get("ForceIntegerScaling", true),
		prefs.Get("BlurryScaling", false),
	};
	auto graveExitsConsole = prefs.Get("GraveExitsConsole", true);
	momentumScroll = prefs.Get("MomentumScroll", true);
	showAvatars = prefs.Get("ShowAvatars", true);

	auto true_string = [](ByteString str) {
		str = str.ToLower();
		return str == "true" ||
		       str == "t" ||
		       str == "on" ||
		       str == "yes" ||
		       str == "y" ||
		       str == "1" ||
		       str == ""; // standalone "redirect" or "disable-bluescreen" or similar arguments
	};
	auto true_arg = [&true_string](Argument arg) {
		return arg.has_value() && true_string(arg.value());
	};

	auto kioskArg = arguments["kiosk"];
	if (kioskArg.has_value())
	{
		windowFrameOps.fullscreen = true_string(kioskArg.value());
		prefs.Set("Fullscreen", windowFrameOps.fullscreen);
	}

	if (true_arg(arguments["redirect"]))
	{
		FILE *new_stdout = freopen("stdout.log", "w", stdout);
		FILE *new_stderr = freopen("stderr.log", "w", stderr);
		if (!new_stdout || !new_stderr)
		{
			Platform::Exit(42);
		}
	}

	auto scaleArg = arguments["scale"];
	if (scaleArg.has_value())
	{
		try
		{
			windowFrameOps.scale = scaleArg.value().ToNumber<int>();
			prefs.Set("Scale", windowFrameOps.scale);
		}
		catch (const std::runtime_error &e)
		{
			std::cerr << "failed to set scale: " << e.what() << std::endl;
		}
	}

	auto clientConfig = [&prefs](Argument arg, ByteString name, ByteString defaultValue) {
		ByteString value;
		if (arg.has_value())
		{
			value = arg.value();
			if (value == "")
			{
				value = defaultValue;
			}
			prefs.Set(name, value);
		}
		else
		{
			value = prefs.Get(name, defaultValue);
		}
		return value;
	};
	ByteString proxyString = clientConfig(arguments["proxy"], "Proxy", "");
	ByteString cafileString = clientConfig(arguments["cafile"], "CAFile", "");
	ByteString capathString = clientConfig(arguments["capath"], "CAPath", "");
	bool disableNetwork = true_arg(arguments["disable-network"]);
	explicitSingletons->requestManager = http::RequestManager::Create(proxyString, cafileString, capathString, disableNetwork);

	explicitSingletons->client = std::make_unique<Client>();
	Client::Ref().Initialize();

	explicitSingletons->saveRenderer = std::make_unique<SaveRenderer>();
	explicitSingletons->favorite = std::make_unique<Favorite>();
	explicitSingletons->engine = std::make_unique<ui::Engine>();

	// TODO: maybe bind the maximum allowed scale to screen size somehow
	if(windowFrameOps.scale < 1 || windowFrameOps.scale > SCALE_MAXIMUM)
		windowFrameOps.scale = 1;

	auto &engine = ui::Engine::Ref();
	engine.g = new Graphics();
	engine.GraveExitsConsole = graveExitsConsole;
	engine.MomentumScroll = momentumScroll;
	engine.ShowAvatars = showAvatars;
	engine.Begin();
	engine.SetFastQuit(prefs.Get("FastQuit", true));
	engine.TouchUI = prefs.Get("TouchUI", DEFAULT_TOUCH_UI);
	engine.windowFrameOps = windowFrameOps;

	SDLOpen();

	if (Client::Ref().IsFirstRun() && FORCE_WINDOW_FRAME_OPS == forceWindowFrameOpsNone)
	{
		auto guessed = GuessBestScale();
		if (engine.windowFrameOps.scale != guessed)
		{
			engine.windowFrameOps.scale = guessed;
			prefs.Set("Scale", windowFrameOps.scale);
			showLargeScreenDialog = true;
		}
	}

	bool enableBluescreen = USE_BLUESCREEN && !true_arg(arguments["disable-bluescreen"]);
	if (enableBluescreen)
	{
		//Get ready to catch any dodgy errors
		for (auto *msg = signalMessages; msg->message; ++msg)
		{
			signal(msg->sig, SigHandler);
		}
		std::set_terminate(TerminateHandler);
	}

	if constexpr (X86)
	{
		X86KillDenormals();
	}

	explicitSingletons->simulationData = std::make_unique<SimulationData>();
	explicitSingletons->gameController = std::make_unique<GameController>();
	auto *gameController = explicitSingletons->gameController.get();
	engine.ShowWindow(gameController->GetView());

	auto openArg = arguments["open"];
	if (openArg.has_value())
	{
		if constexpr (DEBUG)
		{
			std::cout << openArg.value() << " 불러오는 중..." << std::endl;
		}
		if (Platform::FileExists(openArg.value()))
		{
			try
			{
				std::vector<char> gameSaveData;
				if (!Platform::ReadFile(gameSaveData, openArg.value()))
				{
					new ErrorMessage("Error", "파일을 읽을 수 없습니다.");
				}
				else
				{
					auto newFile = std::make_unique<SaveFile>(openArg.value());
					auto newSave = std::make_unique<GameSave>(std::move(gameSaveData));
					newFile->SetGameSave(std::move(newSave));
					gameController->LoadSaveFile(std::move(newFile));
				}

			}
			catch (std::exception & e)
			{
				new ErrorMessage("Error", "세이브파일을 불러오는데 실패했습니다:\n" + ByteString(e.what()).FromUtf8()) ;
			}
		}
		else
		{
			new ErrorMessage("Error", "파일을 열 수 없습니다.");
		}
	}

	auto ptsaveArg = arguments["ptsave"];
	if (ptsaveArg.has_value())
	{
		engine.g->Clear();
		engine.g->DrawRect(RectSized(engine.g->Size() / 2 - Vec2(100, 25), Vec2(200, 50)), 0xB4B4B4_rgb);
		String loadingText = "Loading save...";
		engine.g->BlendText(engine.g->Size() / 2 - Vec2((Graphics::TextSize(loadingText).X - 1) / 2, 5), loadingText, style::Colour::InformationTitle);

		blit(engine.g->Data());
		try
		{
			ByteString saveIdPart;
			if (ByteString::Split split = ptsaveArg.value().SplitBy(':'))
			{
				if (split.Before() != "ptsave")
					throw std::runtime_error("ptsave가 아닙니다.");
				saveIdPart = split.After().SplitBy('#').Before();
			}
			else
				throw std::runtime_error("잘못된 세이브 링크");

			if (!saveIdPart.size())
				throw std::runtime_error("세이브 ID가 없습니다.");
			if constexpr (DEBUG)
			{
				std::cout << "Got Ptsave: id: " << saveIdPart << std::endl;
			}
			ByteString saveHistoryPart = "0";
			if (auto split = saveIdPart.SplitBy('@'))
			{
				saveHistoryPart = split.After();
				saveIdPart = split.Before();
			}
			int saveId = saveIdPart.ToNumber<int>();
			int saveHistory = saveHistoryPart.ToNumber<int>();
			gameController->OpenSavePreview(saveId, saveHistory, savePreviewUrl);
		}
		catch (std::exception & e)
		{
			new ErrorMessage("Error", ByteString(e.what()).FromUtf8());
			Platform::MarkPresentable();
		}
	}
	else
	{
		Platform::MarkPresentable();
	}

	MainLoop();

	Platform::Exit(0);
	return 0;
}