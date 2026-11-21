#include "32blit.hpp"
#include "assets.hpp"
#include "paddle.hpp"
#include "scene.hpp"
#include "sprite_render.hpp"
#include "tileset_descriptor.hpp"
#ifdef BLIT_DEV_BRIDGE
#include "bridge.hpp"
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <utility>

using namespace blit;
using sdk::BoxCollider2D;
using sdk::Sprite;
using sdk::Transform2D;

namespace {
constexpr int width = 320, height = 240;
constexpr float court_top = 24, court_bottom = 216;
constexpr float tick_seconds = 0.010f; // The SDK runs update() every 10 ms.
constexpr float ball_speed = 190.0f;
constexpr char scene_path[] = "scenes/pong.scene";
constexpr char map_path[] = "scenes/pong.map";

sdk::World world(16);
std::map<std::string, std::string, std::less<>> scene_text;

struct PackedSurfaceDeleter {
  void operator()(Surface *surface) const {
    delete[] surface->data;
    delete[] surface->palette;
    delete surface;
  }
};
std::unique_ptr<Surface, PackedSurfaceDeleter> court_sheet, paddle_sheet, ball_sheet;

struct Art {
  std::unique_ptr<Map> court;
  sdk::SpriteClips paddle, ball;
};
Art art, staged_art;
Transform2D serve_position;
float vx = 0, vy = 0;
bool waiting = true;
int serve_direction = -1;
unsigned player_score = 0, cpu_score = 0;

bool read_bytes(const std::string &path, std::string &bytes) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto length = file.tellg();
  if (length < 0) return false;
  bytes.resize(static_cast<std::size_t>(length));
  return static_cast<bool>(file.seekg(0).read(bytes.data(), static_cast<std::streamsize>(length)));
}

bool read_clips(const char *path, const char *image, const Surface &sheet, sdk::SpriteClips &clips) {
  std::ifstream file(path);
  const auto json = nlohmann::json::parse(file, nullptr, false);
  const char *error = sdk::read_sprite_clips(json, clips);
  if (!error && (clips.image != image || sheet.bounds.w % clips.frame_width || sheet.bounds.h % clips.frame_height))
    error = "expected the corresponding packed PNG and a matching frame grid";
  if (!error) {
    const unsigned cells = (sheet.bounds.w / clips.frame_width) * (sheet.bounds.h / clips.frame_height);
    for (const auto &entry : clips.clips)
      for (const auto &frame : entry.second.frames)
        if (frame.cell >= cells) error = "frame outside the sheet";
  }
  if (error) std::fprintf(stderr, "%s: %s\n", path, error);
  return !error;
}

bool read_scene_source(void *, std::string_view path, std::string_view &text) {
  auto &entry = *scene_text.try_emplace(std::string(path)).first;
  if (!read_bytes(entry.first, entry.second)) return false;
  text = entry.second;
  if (path != scene_path) return true;

  std::string bytes;
  if (read_bytes(map_path, bytes))
    staged_art.court = load_tiled_map(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
  if (!staged_art.court) {
    std::fprintf(stderr, "%s: cannot read native map\n", map_path);
    return false;
  }
  std::ifstream link(std::string(map_path) + ".json");
  const auto association = nlohmann::json::parse(link, nullptr, false);
  if(!association.is_object() || !association.contains("tileset") || association["tileset"] != "assets/court.tileset.json") {
    std::fprintf(stderr, "%s.json: expected the Pong court tileset association\n", map_path);
    return false;
  }
  std::ifstream tileset_file("assets/court.tileset.json");
  sdk::TilesetDescriptor tileset;
  if(sdk::read_tileset_descriptor(nlohmann::json::parse(tileset_file, nullptr, false), tileset) ||
     tileset.image != "assets/court.png" ||
     !atlas_tile_count(court_sheet->bounds.w, court_sheet->bounds.h, tileset.tile_size) ||
     !staged_art.court->set_tile_size(tileset.tile_size)) {
    std::fprintf(stderr, "assets/court.tileset.json: invalid packed court geometry\n");
    return false;
  }
  return read_clips("assets/paddle.sprite.json", "assets/paddles.png", *paddle_sheet, staged_art.paddle) &&
         read_clips("assets/ball.sprite.json", "assets/ball.png", *ball_sheet, staged_art.ball);
}

void finish_scene(void *, bool success) noexcept {
  if (success) {
    art = std::move(staged_art);
    if (const auto *ball = world.get<Transform2D>(world.find("ball"))) serve_position = *ball;
    waiting = true;
    serve_direction = -1;
    player_score = cpu_score = 0;
  }
  staged_art = {};
  scene_text.clear();
}

struct Box { float left, top, right, bottom; };

Box bounds(const Transform2D &pos, const BoxCollider2D &box) {
  const float left = pos.x + box.offset_x, top = pos.y + box.offset_y;
  return {left, top, left + box.width, top + box.height};
}

float center_y(Box box) { return (box.top + box.bottom) * 0.5f; }

void draw_court(Map &map) {
  for(const auto &name : map.layer_order)
    map.layers.at(name).draw(screen, *court_sheet, Rect(0, 0, width, height));
}
} // namespace

void init() {
  set_screen_mode(ScreenMode::hires, Size(width, height));
  static sdk::ComponentPool<sdk::Name> names(world, world.capacity());
  static sdk::ComponentPool<Transform2D> transforms(world, world.capacity());
  static sdk::ComponentPool<Sprite> sprites(world, world.capacity());
  static sdk::ComponentPool<BoxCollider2D> colliders(world, world.capacity());
  static sdk::ComponentPool<PaddleDrive> drives(world, world.capacity());
  static sdk::SceneRuntime scenes(world, {24, 72, 160, 8}, {nullptr, read_scene_source, finish_scene});

  court_sheet.reset(Surface::load(packed_court));
  paddle_sheet.reset(Surface::load(packed_paddles));
  ball_sheet.reset(Surface::load(packed_ball));
  if (!court_sheet || !paddle_sheet || !ball_sheet) return;
  sdk::SceneError error;
  if (!scenes.load(scene_path, error))
    std::fprintf(stderr, "%s:%u: %s\n", error.file, error.line, error.message);
#ifdef BLIT_DEV_BRIDGE
  sdk::bridge_attach(world, &scenes);
#endif
}

void update(uint32_t) {
  const auto ball = world.find("ball");
  auto *pos = world.get<Transform2D>(ball);
  const auto *shape = world.get<BoxCollider2D>(ball);
  if (!pos || !shape || shape->width <= 0 || shape->height <= 0 ||
      shape->height > court_bottom - court_top) return;

  if (buttons.pressed & Button::B) {
    player_score = cpu_score = 0;
    serve_direction = -1;
    *pos = serve_position;
    waiting = true;
    return;
  }
  if (waiting && (buttons.pressed & Button::A)) {
    vx = serve_direction * ball_speed;
    vy = (player_score + cpu_score) % 2 ? -74.0f : 74.0f;
    waiting = false;
  }

  const auto before = bounds(*pos, *shape);
  if (!waiting) {
    pos->x += vx * tick_seconds;
    pos->y += vy * tick_seconds;
    const float top = court_top - shape->offset_y;
    const float bottom = court_bottom - shape->offset_y - shape->height;
    if (pos->y < top && vy < 0) { pos->y = top; vy = -vy; }
    if (pos->y > bottom && vy > 0) { pos->y = bottom; vy = -vy; }
  }
  const auto next = bounds(*pos, *shape);
  const bool moving_right = vx > 0;
  bool bounced = false;
  const int input = bool(buttons & Button::DPAD_DOWN) - bool(buttons & Button::DPAD_UP);

  world.each<Transform2D, BoxCollider2D, PaddleDrive>([&](sdk::Entity, Transform2D &paddle,
                                                        const BoxCollider2D &box, const PaddleDrive &drive) {
    if (box.width <= 0 || box.height <= 0 || box.height > court_bottom - court_top) return;
    const float step = std::max(0.0f, drive.speed) * tick_seconds;
    paddle.y += drive.computer ? std::clamp(center_y(next) - center_y(bounds(paddle, box)), -step, step)
                               : input * step;
    paddle.y = std::clamp(paddle.y, court_top - box.offset_y, court_bottom - box.offset_y - box.height);
    if (waiting || bounced) return;

    // Test the leading ball edge crossing the inward paddle face.
    const auto target = bounds(paddle, box);
    const float face = moving_right ? target.left : target.right;
    const bool crossed = moving_right ? before.right <= face && next.right >= face
                                      : before.left >= face && next.left <= face;
    if (crossed && next.top < target.bottom && next.bottom > target.top) {
      bounced = true;
      pos->x += face - (moving_right ? next.right : next.left);
      vx = -vx;
      const float hit = (center_y(next) - center_y(target)) / (box.height * 0.5f);
      vy = std::clamp(hit, -1.0f, 1.0f) * 150.0f;
    }
  });

  if (waiting) return;
  const auto result = bounds(*pos, *shape);
  if (result.right < 0) { ++cpu_score; serve_direction = -1; }
  else if (result.left > width) { ++player_score; serve_direction = 1; }
  else return;
  *pos = serve_position;
  waiting = true;
}

void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  if (!art.court) {
    screen.pen = Pen(255, 224, 151);
    screen.text("Start the game from the team-pong project folder.", minimal_font, Point(12, 12));
    return;
  }
  draw_court(*art.court);
  world.each<Transform2D, Sprite>([&](sdk::Entity, const Transform2D &pos, const Sprite &sprite) {
    const std::string_view asset = sprite.asset;
    if (asset == "assets/paddle") sdk::draw_sprite(screen, *paddle_sheet, art.paddle, pos, sprite, Point(0, 0), time);
    else if (asset == "assets/ball") sdk::draw_sprite(screen, *ball_sheet, art.ball, pos, sprite, Point(0, 0), time);
  });
  char hud[64];
  std::snprintf(hud, sizeof(hud), "YOU %u   TEAM PONG   %u CPU", player_score, cpu_score);
  screen.pen = Pen(208, 248, 248);
  screen.text(hud, fat_font, Point(width / 2, 3), true, TextAlign::top_center);
  screen.text("UP / DOWN: MOVE   Z: SERVE   X: RESET", minimal_font, Point(width / 2, 229), true, TextAlign::top_center);
  if (waiting) screen.text("PRESS Z TO SERVE", fat_font, Point(width / 2, 95), true, TextAlign::top_center);
}
