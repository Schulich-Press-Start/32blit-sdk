/**
 * \file user_code.hpp
 * \brief Symbols that every game provides to the Linux port.
 *
 * A 32blit game is a set of three functions plus some metadata strings.
 * The port calls these; the game never calls into the port directly
 * (it goes through the engine and `blit::api` instead).
 *
 * The metadata strings are generated at build time by `blit_metadata()`
 * (see CMakeLists.txt) from the game's `metadata.yml`. If a game has no
 * metadata, the weak defaults in default_metadata.cpp are used.
 */
#pragma once

#include <cstdint>

/** \brief Called once before the first update. Set screen mode, load assets. */
void init();

/**
 * \brief Game logic, called by `blit::tick()` on a fixed 10 ms step.
 * \param time Milliseconds since the port started.
 */
void update(uint32_t time);

/**
 * \brief Drawing, called once per displayed frame. Draws into `blit::screen`.
 * \param time Milliseconds since the port started.
 */
void render(uint32_t time);

/** \name Game metadata (from metadata.yml) */
///@{
extern const char *metadata_title;
extern const char *metadata_author;
extern const char *metadata_description;
extern const char *metadata_version;
extern const char *metadata_url;
extern const char *metadata_category;
///@}
