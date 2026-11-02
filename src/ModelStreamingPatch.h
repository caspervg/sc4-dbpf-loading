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
	// the view center, and the main thread swaps the occupants to the new models a backlog at a time.
	// On modern hardware the load area covers the whole city, the loader waits for the main thread
	// most of the time, and every frame that applies cells redraws a large part of the static view.
	//
	// This patch limits the load area to the view and a margin around it, keeps the loader thread
	// working, and spreads both the model swaps and a single redraw of the view over frames within a
	// fixed per-frame time budget.
	void Install();
}
