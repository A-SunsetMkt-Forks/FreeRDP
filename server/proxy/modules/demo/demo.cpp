/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Proxy Server Demo C++ Module
 *
 * Copyright 2019 Kobi Mizrachi <kmizrachi18@gmail.com>
 * Copyright 2021 Armin Novak <anovak@thincast.com>
 * Copyright 2021 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>

#include <freerdp/api.h>
#include <freerdp/scancode.h>
#include <freerdp/server/proxy/proxy_modules_api.h>

#define TAG MODULE_TAG("demo")

struct demo_custom_data
{
	proxyPluginsManager* mgr;
	int somesetting;
};

static constexpr char plugin_name[] = "demo";
static constexpr char plugin_desc[] = "this is a test plugin";

static BOOL demo_plugin_unload([[maybe_unused]] proxyPlugin* plugin)
{
	WINPR_ASSERT(plugin);

	std::cout << "C++ demo plugin: unloading..." << std::endl;

	/* Here we have to free up our custom data storage. */
	if (plugin)
		delete static_cast<struct demo_custom_data*>(plugin->custom);

	return TRUE;
}

static BOOL demo_client_init_connect([[maybe_unused]] proxyPlugin* plugin,
                                     [[maybe_unused]] proxyData* pdata,
                                     [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_uninit_connect([[maybe_unused]] proxyPlugin* plugin,
                                       [[maybe_unused]] proxyData* pdata,
                                       [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_pre_connect([[maybe_unused]] proxyPlugin* plugin,
                                    [[maybe_unused]] proxyData* pdata,
                                    [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_post_connect([[maybe_unused]] proxyPlugin* plugin,
                                     [[maybe_unused]] proxyData* pdata,
                                     [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_post_disconnect([[maybe_unused]] proxyPlugin* plugin,
                                        [[maybe_unused]] proxyData* pdata,
                                        [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_x509_certificate([[maybe_unused]] proxyPlugin* plugin,
                                         [[maybe_unused]] proxyData* pdata,
                                         [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_login_failure([[maybe_unused]] proxyPlugin* plugin,
                                      [[maybe_unused]] proxyData* pdata,
                                      [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_end_paint([[maybe_unused]] proxyPlugin* plugin,
                                  [[maybe_unused]] proxyData* pdata, [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_client_redirect([[maybe_unused]] proxyPlugin* plugin,
                                 [[maybe_unused]] proxyData* pdata, [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_server_post_connect([[maybe_unused]] proxyPlugin* plugin,
                                     [[maybe_unused]] proxyData* pdata,
                                     [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_server_peer_activate([[maybe_unused]] proxyPlugin* plugin,
                                      [[maybe_unused]] proxyData* pdata,
                                      [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_server_channels_init([[maybe_unused]] proxyPlugin* plugin,
                                      [[maybe_unused]] proxyData* pdata,
                                      [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_server_channels_free([[maybe_unused]] proxyPlugin* plugin,
                                      [[maybe_unused]] proxyData* pdata,
                                      [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_server_session_end([[maybe_unused]] proxyPlugin* plugin,
                                    [[maybe_unused]] proxyData* pdata,
                                    [[maybe_unused]] void* custom)
{
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(custom);

	WLog_INFO(TAG, "called");
	return TRUE;
}

static BOOL demo_filter_keyboard_event([[maybe_unused]] proxyPlugin* plugin,
                                       [[maybe_unused]] proxyData* pdata,
                                       [[maybe_unused]] void* param)
{
	proxyPluginsManager* mgr = nullptr;
	auto event_data = static_cast<const proxyKeyboardEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	mgr = plugin->mgr;
	WINPR_ASSERT(mgr);

	if (event_data == nullptr)
		return FALSE;

	if (event_data->rdp_scan_code == RDP_SCANCODE_KEY_B)
	{
		/* user typed 'B', that means bye :) */
		std::cout << "C++ demo plugin: aborting connection" << std::endl;
		mgr->AbortConnect(mgr, pdata);
	}

	return TRUE;
}

static BOOL demo_filter_unicode_event([[maybe_unused]] proxyPlugin* plugin,
                                      [[maybe_unused]] proxyData* pdata,
                                      [[maybe_unused]] void* param)
{
	proxyPluginsManager* mgr = nullptr;
	auto event_data = static_cast<const proxyUnicodeEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	mgr = plugin->mgr;
	WINPR_ASSERT(mgr);

	if (event_data == nullptr)
		return FALSE;

	if (event_data->code == 'b')
	{
		/* user typed 'B', that means bye :) */
		std::cout << "C++ demo plugin: aborting connection" << std::endl;
		mgr->AbortConnect(mgr, pdata);
	}

	return TRUE;
}

static BOOL demo_mouse_event([[maybe_unused]] proxyPlugin* plugin,
                             [[maybe_unused]] proxyData* pdata, [[maybe_unused]] void* param)
{
	auto event_data = static_cast<const proxyMouseEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WLog_INFO(TAG, "called %p", event_data);
	return TRUE;
}

static BOOL demo_mouse_ex_event([[maybe_unused]] proxyPlugin* plugin,
                                [[maybe_unused]] proxyData* pdata, [[maybe_unused]] void* param)
{
	auto event_data = static_cast<const proxyMouseExEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WLog_INFO(TAG, "called %p", event_data);
	return TRUE;
}

static BOOL demo_client_channel_data([[maybe_unused]] proxyPlugin* plugin,
                                     [[maybe_unused]] proxyData* pdata,
                                     [[maybe_unused]] void* param)
{
	const auto* channel = static_cast<const proxyChannelDataEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	WLog_INFO(TAG, "%s [0x%04" PRIx16 "] got %" PRIuz, channel->channel_name, channel->channel_id,
	          channel->data_len);
	return TRUE;
}

static BOOL demo_server_channel_data([[maybe_unused]] proxyPlugin* plugin,
                                     [[maybe_unused]] proxyData* pdata,
                                     [[maybe_unused]] void* param)
{
	const auto* channel = static_cast<const proxyChannelDataEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	WLog_WARN(TAG, "%s [0x%04" PRIx16 "] got %" PRIuz, channel->channel_name, channel->channel_id,
	          channel->data_len);
	return TRUE;
}

static BOOL demo_dynamic_channel_create([[maybe_unused]] proxyPlugin* plugin,
                                        [[maybe_unused]] proxyData* pdata,
                                        [[maybe_unused]] void* param)
{
	const auto* channel = static_cast<const proxyChannelDataEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(channel);

	WLog_WARN(TAG, "%s [0x%04" PRIx16 "]", channel->channel_name, channel->channel_id);
	return TRUE;
}

static BOOL demo_server_fetch_target_addr([[maybe_unused]] proxyPlugin* plugin,
                                          [[maybe_unused]] proxyData* pdata,
                                          [[maybe_unused]] void* param)
{
	auto event_data = static_cast<const proxyFetchTargetEventInfo*>(param);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(event_data);

	WLog_INFO(TAG, "called %p", event_data);
	return TRUE;
}

static BOOL demo_server_peer_logon([[maybe_unused]] proxyPlugin* plugin,
                                   [[maybe_unused]] proxyData* pdata, [[maybe_unused]] void* param)
{
	auto info = static_cast<const proxyServerPeerLogon*>(param);
	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(info);
	WINPR_ASSERT(info->identity);

	WLog_INFO(TAG, "%d", info->automatic);
	return TRUE;
}

static BOOL demo_dyn_channel_intercept_list([[maybe_unused]] proxyPlugin* plugin,
                                            [[maybe_unused]] proxyData* pdata,
                                            [[maybe_unused]] void* arg)
{
	auto data = static_cast<proxyChannelToInterceptData*>(arg);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(data);

	WLog_INFO(TAG, "%s: %p", __func__, data);
	return TRUE;
}

static BOOL demo_static_channel_intercept_list([[maybe_unused]] proxyPlugin* plugin,
                                               [[maybe_unused]] proxyData* pdata,
                                               [[maybe_unused]] void* arg)
{
	auto data = static_cast<proxyChannelToInterceptData*>(arg);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(data);

	WLog_INFO(TAG, "%s: %p", __func__, data);
	return TRUE;
}

static BOOL demo_dyn_channel_intercept([[maybe_unused]] proxyPlugin* plugin,
                                       [[maybe_unused]] proxyData* pdata,
                                       [[maybe_unused]] void* arg)
{
	auto data = static_cast<proxyDynChannelInterceptData*>(arg);

	WINPR_ASSERT(plugin);
	WINPR_ASSERT(pdata);
	WINPR_ASSERT(data);

	WLog_INFO(TAG, "%s: %p", __func__, data);
	return TRUE;
}

#ifdef __cplusplus
extern "C"
{
#endif
	FREERDP_API BOOL proxy_module_entry_point(proxyPluginsManager* plugins_manager, void* userdata);
#ifdef __cplusplus
}
#endif

BOOL proxy_module_entry_point(proxyPluginsManager* plugins_manager, void* userdata)
{
	struct demo_custom_data* custom = nullptr;
	proxyPlugin plugin = {};

	plugin.name = plugin_name;
	plugin.description = plugin_desc;
	plugin.PluginUnload = demo_plugin_unload;
	plugin.ClientInitConnect = demo_client_init_connect;
	plugin.ClientUninitConnect = demo_client_uninit_connect;
	plugin.ClientPreConnect = demo_client_pre_connect;
	plugin.ClientPostConnect = demo_client_post_connect;
	plugin.ClientPostDisconnect = demo_client_post_disconnect;
	plugin.ClientX509Certificate = demo_client_x509_certificate;
	plugin.ClientLoginFailure = demo_client_login_failure;
	plugin.ClientEndPaint = demo_client_end_paint;
	plugin.ClientRedirect = demo_client_redirect;
	plugin.ServerPostConnect = demo_server_post_connect;
	plugin.ServerPeerActivate = demo_server_peer_activate;
	plugin.ServerChannelsInit = demo_server_channels_init;
	plugin.ServerChannelsFree = demo_server_channels_free;
	plugin.ServerSessionEnd = demo_server_session_end;
	plugin.KeyboardEvent = demo_filter_keyboard_event;
	plugin.UnicodeEvent = demo_filter_unicode_event;
	plugin.MouseEvent = demo_mouse_event;
	plugin.MouseExEvent = demo_mouse_ex_event;
	plugin.ClientChannelData = demo_client_channel_data;
	plugin.ServerChannelData = demo_server_channel_data;
	plugin.DynamicChannelCreate = demo_dynamic_channel_create;
	plugin.ServerFetchTargetAddr = demo_server_fetch_target_addr;
	plugin.ServerPeerLogon = demo_server_peer_logon;

	plugin.StaticChannelToIntercept = demo_static_channel_intercept_list;
	plugin.DynChannelToIntercept = demo_dyn_channel_intercept_list;
	plugin.DynChannelIntercept = demo_dyn_channel_intercept;

	plugin.userdata = userdata;

	custom = new (struct demo_custom_data);
	if (!custom)
		return FALSE;

	custom->mgr = plugins_manager;
	custom->somesetting = 42;

	plugin.custom = custom;
	plugin.userdata = userdata;

	return plugins_manager->RegisterPlugin(plugins_manager, &plugin);
}
