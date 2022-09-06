/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#include <freerdp/config.h>

#include <winpr/assert.h>

#include "wf_channels.h"

#include "wf_rail.h"
#include "wf_cliprdr.h"

#include <freerdp/gdi/gfx.h>
#include <freerdp/gdi/video.h>

#include <freerdp/log.h>
#define TAG CLIENT_TAG("windows")

BOOL encomsp_request_control(wfContext* wf, BOOL request)
{
	ENCOMSP_CHANGE_PARTICIPANT_CONTROL_LEVEL_PDU pdu;

	EncomspClientContext* encomsp = wf->encomsp;

	if (!encomsp)
		return FALSE;

	// wfContext* wf = (wfContext*)encomsp->custom;
	if (!wf->common.context.settings->RemoteAssistanceMode)
		return FALSE;

	pdu.ParticipantId = 0;
	pdu.Flags = ENCOMSP_REQUEST_VIEW;

	if (request)
		pdu.Flags |= ENCOMSP_REQUEST_INTERACT;

	encomsp->ChangeParticipantControlLevel(encomsp, &pdu);
	return TRUE;
}

BOOL encomsp_toggle_control(EncomspClientContext* encomsp)
{
	wfContext* wf = (wfContext*)encomsp->custom;

	encomsp_request_control(wf, !wf->controlToggle);

	wf->controlToggle = !wf->controlToggle;
}

static UINT
wf_encomsp_participant_created(EncomspClientContext* context,
                               const ENCOMSP_PARTICIPANT_CREATED_PDU* participantCreated)
{
	WLog_DBG(TAG, "wf_encomsp_participant_created");

	wfContext* wf;
	rdpSettings* settings;
	static BOOL requestedOnce = FALSE;

	if (!context || !context->custom || !participantCreated)
		return ERROR_INVALID_PARAMETER;

	wf = (wfContext*)context->custom;
	WINPR_ASSERT(wf);

	settings = wf->common.context.settings;

	if (!settings)
		return ERROR_INVALID_PARAMETER;

	if ((participantCreated->Flags & ENCOMSP_MAY_VIEW) &&
	    !(participantCreated->Flags & ENCOMSP_MAY_INTERACT))
	{
		// if auto-request-control setting is enabled then only request control once upon connect,
		// otherwise it will request control again every time server turns off control which is a
		// bit annoying
		if (!requestedOnce &&
		    freerdp_settings_get_bool(settings, FreeRDP_RemoteAssistanceRequestControl))
		{
			requestedOnce = TRUE;
			if (!encomsp_toggle_control(context))
				return ERROR_INTERNAL_ERROR;
		}
	}
	else if (participantCreated->Flags & ENCOMSP_MAY_INTERACT)
	{
		WLog_DBG(TAG, "wf_encomsp_participant_created ENCOMSP_MAY_INTERACT");
	}

	return CHANNEL_RC_OK;
}

static void wf_encomsp_init(wfContext* wf, EncomspClientContext* encomsp)
{
	wf->encomsp = encomsp;
	encomsp->custom = (void*)wf;
	encomsp->ParticipantCreated = wf_encomsp_participant_created;
}

static void wf_encomsp_uninit(wfContext* wf, EncomspClientContext* encomsp)
{
	if (encomsp)
	{
		encomsp->custom = NULL;
		encomsp->ParticipantCreated = NULL;
	}

	if (wf)
		wf->encomsp = NULL;
}

void wf_OnChannelConnectedEventHandler(void* context, const ChannelConnectedEventArgs* e)
{
	wfContext* wfc = (wfContext*)context;
	rdpSettings* settings;

	WINPR_ASSERT(wfc);
	WINPR_ASSERT(e);

	settings = wfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
		wf_rail_init(wfc, (RailClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		wf_cliprdr_init(wfc, (CliprdrClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0)
	{
		wf_encomsp_init(wfc, (EncomspClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		wfc->disp = (DispClientContext*)e->pInterface;
	}
	else
		freerdp_client_OnChannelConnectedEventHandler(context, e);
}

void wf_OnChannelDisconnectedEventHandler(void* context, const ChannelDisconnectedEventArgs* e)
{
	wfContext* wfc = (wfContext*)context;
	rdpSettings* settings;

	WINPR_ASSERT(wfc);
	WINPR_ASSERT(e);

	settings = wfc->common.context.settings;
	WINPR_ASSERT(settings);

	if (strcmp(e->name, RAIL_SVC_CHANNEL_NAME) == 0)
	{
		wf_rail_uninit(wfc, (RailClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
	{
		wf_cliprdr_uninit(wfc, (CliprdrClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, ENCOMSP_SVC_CHANNEL_NAME) == 0)
	{
		wf_encomsp_uninit(wfc, (EncomspClientContext*)e->pInterface);
	}
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		wfc->disp = NULL;
	}
	else
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
}
