/*
 * This file is part of sc4-dbpf-loading, a DLL Plugin for SimCity 4 that
 * optimizes the DBPF loading.
 *
 * Copyright (C) 2026 Casper Van Gheluwe
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, under
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

namespace ModelStreamingPatch
{
	// Speeds up the model loading that runs after a zoom or rotation change.
	//
	// SC4's cSC4ModelBackgroundLoader thread loads the models for the new view in a spiral around
	// the view center. It only works a few cells per wake before it sleeps again, and the main
	// thread applies the loaded cells a few at a time. Every frame that applies cells redraws a
	// large part of the static view, so one zoom change can cost dozens of slow frames.
	//
	// This patch lets the loader thread work in real-time slices with short pauses, removes its
	// backlog cap, and has the main thread apply the loaded cells in batches.
	void Install();
}
