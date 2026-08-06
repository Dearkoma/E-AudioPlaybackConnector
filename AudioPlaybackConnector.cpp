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
void RestoreAudioService();

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
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
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
		// Close all active audio connections first
		for (const auto& connection : g_audioPlaybackConnections)
		{
			connection.second.second.Close();
			g_devicePicker.SetDisplayStatus(connection.second.first, {}, DevicePickerDisplayStatusOptions::None);
		}

		if (g_reconnect)
		{
			// Save the device list for reconnect before clearing
			SaveSettings();
		}

		// Brief wait to let Windows complete async audio endpoint cleanup.
		// Without this, the process may exit before the OS releases audio
		// resources, leaving zombie endpoints that require a reboot to recover.
		// 500ms is typically sufficient for the Bluetooth audio stack to finalize.
		Sleep(500);

		if (!g_reconnect)
		{
			SaveSettings();
		}

		g_audioPlaybackConnections.clear();
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
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
	case WM_RESTORESERVICE_RESULT:
	{
		if (wParam == 0)
		{
			TaskDialog(nullptr, nullptr,
				_(L"Bluetooth Audio Restarted"),
				nullptr,
				_(L"Windows Audio and Bluetooth services have been restarted successfully.\n\nPlease reconnect your Bluetooth device now."),
				TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
		}
		else if (wParam == 2)
		{
			TaskDialog(nullptr, nullptr,
				_(L"Service Restart Failed"),
				nullptr,
				_(L"Could not restart the audio services.\n\n"
				  L"This may happen if the elevation prompt was denied.\n\n"
				  L"If audio is still not working, please reboot your computer."),
				TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		}
		else
		{
			TaskDialog(nullptr, nullptr,
				_(L"Service Restart Failed"),
				nullptr,
				_(L"The restart did not complete successfully (the operation may have been cancelled or some services are not available).\n\nIf audio is still not working, please reboot your computer."),
				TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
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
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
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
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
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
	disconnectAllItem.Text(_(L"Disconnect All"));
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
	restartAudioItem.Text(_(L"Restart Bluetooth Audio"));
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
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool success = false;
	std::wstring errorMessage;

	try
	{
		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			g_audioPlaybackConnections.emplace(device.Id(), std::pair(device, connection));

			connection.StateChanged([](const auto& sender, const auto&) {
				if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					auto it = g_audioPlaybackConnections.find(std::wstring(sender.DeviceId()));
					if (it != g_audioPlaybackConnections.end())
					{
						g_devicePicker.SetDisplayStatus(it->second.first, {}, DevicePickerDisplayStatusOptions::None);
						g_audioPlaybackConnections.erase(it);
					}
					// Do NOT call sender.Close() here — the connection is already Closed,
					// and calling Close() again can corrupt the internal state machine,
					// potentially leaving zombie audio endpoints that require a reboot.
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
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				success = false;
				errorMessage = _(L"The operation was denied by the system");
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
			errorMessage = _(L"Unknown error");
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
		picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	}
	else
	{
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			// Close the connection and wait briefly for the async audio
			// endpoint cleanup to complete before removing from the map.
			// Without this delay, a partially-initialized endpoint can
			// become a zombie that blocks future connections.
			it->second.second.Close();
			Sleep(200);
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
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
		auto it = g_audioPlaybackConnections.find(std::wstring(device.Id()));
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			Sleep(200); // Allow async audio endpoint cleanup to complete
			g_audioPlaybackConnections.erase(it);
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
	if (g_audioPlaybackConnections.empty())
	{
		return;
	}

	// Close all active connections
	for (auto& pair : g_audioPlaybackConnections)
	{
		pair.second.second.Close();
		g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
	}

	// Brief wait for async cleanup to propagate before clearing the map
	Sleep(300);

	g_audioPlaybackConnections.clear();

	TaskDialog(nullptr, nullptr,
		_(L"All Devices Disconnected"),
		nullptr,
		_(L"All Bluetooth audio connections have been closed.\n\nYou can now reconnect your devices."),
		TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
}

void RestoreAudioService()
{
	int result = TaskDialog(nullptr, nullptr,
		_(L"Restart Bluetooth Audio"),
		nullptr,
		_(L"This will restart Windows Audio and Bluetooth audio services to recover from stuck or silent connections.\n\n"
		  L"Administrator permission is required.\n\n"
		  L"Only audio services are restarted — other Bluetooth devices (mouse, keyboard, etc.) will NOT be affected.\n\n"
		  L"Current audio connections will be closed. You will need to reconnect your Bluetooth audio device after the restart.\n\n"
		  L"Do you want to continue?"),
		TDCBF_YES_BUTTON | TDCBF_CANCEL_BUTTON, TD_WARNING_ICON, nullptr);

	if (result != IDYES)
	{
		return;
	}

	// Close all connections first so the audio endpoints are released.
	// Do NOT call DisconnectAllDevices() here — it shows its own modal dialog
	// and blocks the UI. Instead, close connections inline.
	for (auto& pair : g_audioPlaybackConnections)
	{
		pair.second.second.Close();
		g_devicePicker.SetDisplayStatus(pair.second.first, {}, DevicePickerDisplayStatusOptions::None);
	}

	g_audioPlaybackConnections.clear();

	// Run the elevated service restart in a background thread to avoid
	// blocking the UI thread. ShellExecuteExW + WaitForSingleObject can
	// take up to 30 seconds, which would freeze the application.
	// The initial Sleep allows async audio endpoint cleanup from Close()
	// to complete before we stop the audio service, preventing a potential
	// deadlock where the service stop waits for endpoints we held.
	std::thread([hwnd = g_hWnd]() {
		Sleep(1000);
		SHELLEXECUTEINFOW sei = {
			.cbSize = sizeof(sei),
			.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
			.hwnd = hwnd,
			.lpVerb = L"runas",
			.lpFile = L"cmd",
			.lpParameters = L"/c "
				L"net stop audiosrv /y 2>nul & "
				L"net stop AudioEndpointBuilder /y 2>nul & "
				L"net stop BTAGService /y 2>nul & "
				L"net stop BthAvctpSvc /y 2>nul & "
				L"net start BthAvctpSvc 2>nul & "
				L"net start BTAGService 2>nul & "
				L"net start AudioEndpointBuilder 2>nul & "
				L"net start audiosrv 2>nul",
			.nShow = SW_HIDE,
		};

		WPARAM wParam = 0;

		if (ShellExecuteExW(&sei))
		{
			if (sei.hProcess)
			{
				WaitForSingleObject(sei.hProcess, 30000);
				DWORD exitCode = 0;
				GetExitCodeProcess(sei.hProcess, &exitCode);
				CloseHandle(sei.hProcess);
				wParam = exitCode;
			}
			else
			{
				wParam = 1; // process handle missing
			}
		}
		else
		{
			wParam = 2; // ShellExecuteExW failed (elevation denied, etc.)
		}

		PostMessageW(hwnd, WM_RESTORESERVICE_RESULT, wParam, 0);
	}).detach();
}
