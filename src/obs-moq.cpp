/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

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

#include <obs-module.h>

#include "moq-output.h"
#include "moq-service.h"

extern "C" {
#include "hang.h"
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-moq", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "OBS MoQ (Media over QUIC) module";
}

bool obs_module_load(void)
{
	// Use RUST_LOG env var for more verbose output
	hang_log_level("info");

	register_moq_output();
	register_moq_service();

	return true;
}
