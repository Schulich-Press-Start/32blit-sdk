#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace blit { struct Surface; }

namespace sdk {
  /// A screen-space layout. The game owns screen activation and action mapping.
  class UiScreen {
  public:
    UiScreen();
    ~UiScreen();

    /// Load a .ui.json file and reset focus. A failed load leaves this screen unchanged.
    bool load(const std::string &path);
    void reset_focus();
    std::string_view focused_id() const;

    /// Supply pressed edges. The sign of dx/dy selects a direction; horizontal wins if both are set.
    /// Takes at most one step before accepting. Returns the activated button ID, or an empty view.
    /// The view is valid until the next successful load or this screen's destruction.
    std::string_view update(int dx, int dy, bool accept);

    /// Draw the centered, unscaled layout inside the destination's bounds and clip.
    void draw(blit::Surface &surface) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
  };
}
