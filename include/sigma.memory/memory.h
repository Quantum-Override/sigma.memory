/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * File: sigma.memory/memory.h
 * Description: Forwarding header — redirects <sigma.memory/memory.h> to the
 *              local development header so that sigma.test/sigtest.h (which
 *              includes <sigma.memory/memory.h>) resolves to the in-tree
 *              header instead of the installed system copy, preventing
 *              double-definition conflicts during test compilation.
 */
#pragma once

#include "../memory.h"
