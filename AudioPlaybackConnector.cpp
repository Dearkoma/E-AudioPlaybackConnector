#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
void DisconnectAllDevices();
winrt::fire_and_forget RestoreAudioService();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"操作系统不支持"), nullptr, _(L"此操作系统版本不支持 AudioPlaybackConnector。"), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
	{
		// Signal all async operations to stop — prevents pending
		// coroutines (ConnectDevice, RestoreAudioService) from
		// touching resources we are about to release.
		g_shuttingDown = true;

		// Save settings while we still have the connection list intact
		SaveSettings();

		// Close all audio connections and release endpoints.
		// Each Close() triggers async cleanup via StateChanged →
		// WM_CONNECTION_CLOSED, but since we clear the map after
		// closing, those messages will find nothing and be harmless.
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			for (auto& pair : g_audioPlaybackConnections)
			{
				pair.second.second.Close();
				g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
			}
			g_audioPlaybackConnections.clear();
		}

		// Remove tray icon
		Shell_NotifyIconW(NIM_DELETE, &g_nid);

		// Allow time for async audio endpoint cleanup to propagate
		// before the process exits. Without this delay, the OS may
		// not release endpoints in time, leaving zombie entries that
		// require a reboot to recover.
		Sleep(500);

		PostQuitMessage(0);
		break;
	}
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(g_devicePicker, i);
			}
			g_lastDevices.clear();
		}
		break;
	case WM_CONNECTION_CLOSED:
	{
		// StateChanged callback runs on an audio/Bluetooth background thread.
		// It posts this message so all XAML UI updates and map mutations happen
		// on the main UI thread, avoiding cross-thread races.
		std::unique_ptr<std::wstring> deviceId(reinterpret_cast<std::wstring*>(wParam));
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		auto it = g_audioPlaybackConnections.find(*deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
			g_audioPlaybackConnections.erase(it);
		}
		break;
	}
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"所有连接将被关闭。\n确定退出吗？"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"下次启动时重新连接")));

	Button button;
	button.Content(winrt::box_value(_(L"退出")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"蓝牙设置"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"退出"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		bool hasConnections;
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			hasConnections = !g_audioPlaybackConnections.empty();
		}
		if (!hasConnections)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);

	// --- Disconnect All ---
	MenuFlyoutSeparator separator1;

	FontIcon disconnectIcon;
	disconnectIcon.Glyph(L"\xE894"); // DisconnectDrive

	MenuFlyoutItem disconnectAllItem;
	disconnectAllItem.Text(_(L"断开全部"));
	disconnectAllItem.Icon(disconnectIcon);
	disconnectAllItem.Click([](const auto&, const auto&) {
		DisconnectAllDevices();
	});

	menu.Items().Append(separator1);
	menu.Items().Append(disconnectAllItem);

	// --- Restart Audio ---
	FontIcon repairIcon;
	repairIcon.Glyph(L"\xE72C"); // Repair

	MenuFlyoutItem restartAudioItem;
	restartAudioItem.Text(_(L"重启蓝牙音频"));
	restartAudioItem.Icon(repairIcon);
	restartAudioItem.Click([](const auto&, const auto&) {
		RestoreAudioService();
	});

	menu.Items().Append(restartAudioItem);

	// --- Separator before Exit ---
	MenuFlyoutSeparator separator2;
	menu.Items().Append(separator2);

	// --- Exit ---
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	if (g_shuttingDown) co_return;

	picker.SetDisplayStatus(device, _(L"正在连接"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			{
				std::lock_guard<std::mutex> lock(g_connectionsMutex);
				g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));
			}

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					// StateChanged fires on a background Bluetooth/audio thread.
					// PostMessage marshals the work to the UI thread so we don't
					// touch XAML objects (g_devicePicker) or the global connection
					// map from the wrong thread.
					auto deviceId = new std::wstring(sender.DeviceId());
					PostMessageW(g_hWnd, WM_CONNECTION_CLOSED, reinterpret_cast<WPARAM>(deviceId), 0);
				}
			});

			co_await connection.StartAsync();
			auto result = co_await connection.OpenAsync();

			switch (result.Status())
			{
			case AudioPlaybackConnectionOpenResultStatus::Success:
				success = true;
				break;
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				success = false;
				errorMessage = _(L"请求超时");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"操作被系统拒绝");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				success = false;
				winrt::throw_hresult(result.ExtendedError());
				break;
			}
		}
		else
		{
			success = false;
			errorMessage = _(L"未知错误");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		success = false;
		errorMessage.resize(64);
		while (1)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (success)
	{
		picker.SetDisplayStatus(device, _(L"已连接"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (it != g_audioPlaybackConnections.end())
			{
				// Close the connection; the StateChanged → WM_CONNECTION_CLOSED
				// path handles the async endpoint cleanup and UI update on the
				// correct thread — no Sleep() needed here.
				it->second.second.Close();
				g_audioPlaybackConnections.erase(it);
			}
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	if (g_shuttingDown) co_return;

	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	ConnectDevice(picker, device);
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		{
			std::lock_guard<std::mutex> lock(g_connectionsMutex);
			auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
			if (it != g_audioPlaybackConnections.end())
			{
				it->second.second.Close();
				g_audioPlaybackConnections.erase(it);
				// StateChanged → WM_CONNECTION_CLOSED handles async
				// endpoint cleanup on the UI thread; no Sleep() needed.
			}
		}
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}

void DisconnectAllDevices()
{
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		if (g_audioPlaybackConnections.empty())
		{
			return;
		}

		// Close all active connections. StateChanged → WM_CONNECTION_CLOSED
		// handles async endpoint cleanup on the UI thread. We clear the map
		// immediately — pending WM_CONNECTION_CLOSED messages will find
		// nothing, which is harmless.
		for (auto& pair : g_audioPlaybackConnections)
		{
			pair.second.second.Close();
			g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}

		g_audioPlaybackConnections.clear();
	}

	TaskDialog(nullptr, nullptr,
		_(L"所有设备已断开"),
		nullptr,
		_(L"所有蓝牙音频连接已关闭。\n\n现在可以重新连接设备。"),
		TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
}

winrt::fire_and_forget RestoreAudioService()
{
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		if (g_audioPlaybackConnections.empty())
		{
			co_return;
		}
	}

	int result = TaskDialog(nullptr, nullptr,
		_(L"重启蓝牙音频"),
		nullptr,
		_(L"这将断开并重新连接由 AudioPlaybackConnector 管理的音频连接。\n\n"
		  L"不会触及其他系统服务 — 仅影响本程序自身的连接。\n"
		  L"其他蓝牙设备（鼠标、键盘等）不会中断。\n\n"
		  L"是否继续？"),
		TDCBF_YES_BUTTON | TDCBF_CANCEL_BUTTON, TD_INFORMATION_ICON, nullptr);

	if (result != IDYES)
	{
		co_return;
	}

	// Save device IDs before closing so we can reconnect
	std::vector<std::wstring> deviceIds;
	{
		std::lock_guard<std::mutex> lock(g_connectionsMutex);
		deviceIds.reserve(g_audioPlaybackConnections.size());
		for (const auto& pair : g_audioPlaybackConnections)
		{
			deviceIds.push_back(pair.first);
			pair.second.second.Close();
			g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}
		g_audioPlaybackConnections.clear();
	}

	// Wait for async audio endpoint cleanup to complete before reconnecting.
	// Without this, the old endpoint may still be releasing resources when
	// we try to open a new one for the same device.
	co_await winrt::resume_after(std::chrono::seconds(1));

	// If the user exited during the wait, don't reconnect
	if (g_shuttingDown) co_return;

	// Reconnect each device with staggered delays to avoid flooding the
	// Bluetooth stack with simultaneous connection requests, which degrades
	// both Bluetooth quality and 2.4 GHz Wi-Fi coexistence.
	for (const auto& id : deviceIds)
	{
		ConnectDevice(g_devicePicker, id);
		co_await winrt::resume_after(std::chrono::milliseconds(500));
	}
}
