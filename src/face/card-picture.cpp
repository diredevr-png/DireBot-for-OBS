/*
DireBot for OBS
Copyright (C) 2026 DireBot

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "card-picture.hpp"

#ifdef _WIN32
#include <d3d11.h>
#endif

namespace direbot::face {

#ifdef _WIN32

namespace {

// The card's own context (released by the caller), or null where OBS isn't
// drawing with Direct3D 11.
ID3D11DeviceContext *context_of(ID3D11Device **device_out = nullptr)
{
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11)
		return nullptr;
	ID3D11Device *device = static_cast<ID3D11Device *>(gs_get_device_obj());
	if (!device)
		return nullptr;
	ID3D11DeviceContext *context = nullptr;
	device->GetImmediateContext(&context);
	if (device_out)
		*device_out = device;
	return context;
}

} // namespace

bool card_picture_put(gs_texture_t *picture)
{
	if (!picture || gs_get_texture_type(picture) != GS_TEXTURE_2D)
		return false;
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = context_of(&device);
	if (!context)
		return false;
	ID3D11Texture2D *texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(picture));
	ID3D11ShaderResourceView *view = nullptr;
	const bool made = texture && SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &view)) && view;
	if (made) {
		context->VSSetShaderResources(0, 1, &view);
		// (The context holds it while it's there.)
		view->Release();
	}
	context->Release();
	return made;
}

void card_picture_take()
{
	ID3D11DeviceContext *context = context_of();
	if (!context)
		return;
	ID3D11ShaderResourceView *none = nullptr;
	context->VSSetShaderResources(0, 1, &none);
	context->Release();
}

#else

bool card_picture_put(gs_texture_t *)
{
	return false;
}

void card_picture_take() {}

#endif

} // namespace direbot::face
