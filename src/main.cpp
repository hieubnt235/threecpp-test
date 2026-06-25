/// threepp spike: a C++ port of the esim-frontend "Simulate" tab editor
/// (create3DViewer.ts + createWiringEditor.ts), proving the C++/WebGL stack
/// reproduces the full interaction: click a device to select + move it with a
/// gizmo, and two-click wiring between connection points -- with the same
/// ON-DEMAND ("render if needed") loop the clone uses.
///
/// Performance parity note: the clone's grid casts NO shadows (only keyLight
/// casts, floor receives) and shares geometry across clones, with a 1024 shadow
/// map. An earlier version of this spike gave all 1000 boxes castShadow +
/// receiveShadow into a 2048 map, which is ~2x the GPU work and never hit 60.
/// This version uses a smaller set of selectable "device" chips sharing one
/// material, a 1024 shadow map, and a true 1-second-window fps reading.

#include "threepp/threepp.hpp"

#include "threepp/controls/OrbitControls.hpp"
#include "threepp/controls/TransformControls.hpp"
#include "threepp/core/Raycaster.hpp"
#include "threepp/input/KeyListener.hpp"
#include "threepp/materials/LineBasicMaterial.hpp"
#include "threepp/objects/Line.hpp"

#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace threepp;

namespace {

    /// Owns the dirty/paused state and the pause-resume scheduling, so the input
    /// callbacks and the loop body share one source of truth. wake() is the C++
    /// equivalent of the clone's requestRenderIfNotRequested(): schedule a frame
    /// only if one is not already pending.
    struct RenderScheduler {
        bool dirty{true};
        bool paused{false};

        void wake() {
            dirty = true;
#ifdef __EMSCRIPTEN__
            if (paused) {
                paused = false;
                emscripten_resume_main_loop();
            }
#endif
        }

        void sleepIfIdle() {
#ifdef __EMSCRIPTEN__
            if (!dirty && !paused) {
                paused = true;
                emscripten_pause_main_loop();
            }
#endif
        }
    };

    /// A connection point on a device: a small sphere marker (child of the body)
    /// plus the local offset used to compute its world position for wires.
    struct Pin {
        Vector3 offset;
        std::shared_ptr<Mesh> marker;
        std::shared_ptr<MeshStandardMaterial> material;
    };

    /// A device = a chip body the user can select + move, with two pins.
    struct Device {
        std::shared_ptr<Mesh> body;
        std::vector<Pin> pins;
    };

    /// A wire between two pins; its 2-point line geometry is refreshed each frame
    /// so it follows the pins when a device is moved.
    struct Wire {
        int deviceA, pinA, deviceB, pinB;
        std::shared_ptr<Line> line;
        std::shared_ptr<BufferGeometry> geometry;
    };

    /// The whole editor: scene authoring, raycast selection, gizmo move, wiring.
    /// It IS the canvas MouseListener so a single object drives every interaction.
    class Editor: public MouseListener {
    public:
        Editor(Scene& scene, PerspectiveCamera& camera, Canvas& canvas,
               TransformControls& gizmo, OrbitControls& orbit, RenderScheduler& scheduler)
            : scene_{scene}, camera_{camera}, canvas_{canvas},
              gizmo_{gizmo}, orbit_{orbit}, scheduler_{scheduler} {}

        /// Build the device grid. One shared green body material (like the clone's
        /// uniform look) keeps material-state churn and draw setup minimal; the
        /// pins get their own material so a pending wire-start can be highlighted.
        void build() {
            const auto bodyGeometry = BoxGeometry::create(0.1f, 0.05f, 0.07f);
            const auto pinGeometry = SphereGeometry::create(0.02f, 8, 6);
            bodyMaterial_ = MeshStandardMaterial::create();
            bodyMaterial_->color = Color{0x4caf50};
            bodyMaterial_->roughness = 0.55f;

            constexpr int cols{40};
            constexpr int rows{25};
            constexpr float spacingX{0.16f};
            constexpr float spacingZ{0.16f};
            const float originX{-spacingX * (cols - 1) / 2.f};
            const float originZ{-spacingZ * (rows - 1) / 2.f};
            const std::array pinOffsets{Vector3{-0.06f, 0.f, 0.f}, Vector3{0.06f, 0.f, 0.f}};

            for (int row{0}; row < rows; ++row) {
                for (int col{0}; col < cols; ++col) {
                    Device device;
                    device.body = Mesh::create(bodyGeometry, bodyMaterial_);
                    device.body->position.set(originX + col * spacingX, 0.025f, originZ + row * spacingZ);
                    device.body->castShadow = false;// match the clone: the 1000 grid casts no shadows (perf)
                    device.body->receiveShadow = false;
                    scene_.add(device.body);

                    const int deviceIndex = static_cast<int>(devices_.size());
                    bodyLookup_.emplace(device.body.get(), deviceIndex);
                    pickables_.push_back(device.body.get());

                    for (int p{0}; p < static_cast<int>(pinOffsets.size()); ++p) {
                        auto material = MeshStandardMaterial::create();
                        material->color = Color{0xffc107};
                        material->roughness = 0.4f;
                        auto marker = Mesh::create(pinGeometry, material);
                        marker->position.copy(pinOffsets[p]);
                        marker->castShadow = false;
                        device.body->add(marker);

                        pinLookup_.emplace(marker.get(), std::pair{deviceIndex, p});
                        pickables_.push_back(marker.get());
                        device.pins.push_back(Pin{pinOffsets[p], marker, material});
                    }
                    devices_.push_back(std::move(device));
                }
            }

            wireMaterial_ = LineBasicMaterial::create();
            wireMaterial_->color = Color{0xff6a00};
        }

        /// prepareRender() equivalent: move each wire's endpoints to its pins'
        /// current world positions, so wires follow devices dragged by the gizmo.
        void refreshWires() {
            for (const auto& wire : wires_) {
                const Vector3 a = pinWorld(wire.deviceA, wire.pinA);
                const Vector3 b = pinWorld(wire.deviceB, wire.pinB);
                auto* position = wire.geometry->getAttribute<float>("position");
                position->setXYZ(0, a.x, a.y, a.z);
                position->setXYZ(1, b.x, b.y, b.z);
                position->needsUpdate();
            }
        }

        /// Set from the gizmo's "mouseDown" event: the press landed on a gizmo
        /// axis, so the matching click in onMouseUp must NOT run scene selection.
        void markGizmoPress() { gizmoPressed_ = true; }

        void cancel() {
            pending_.reset();
            deselect();
            updatePinHighlights();
            scheduler_.wake();
        }

        std::string status() const {
            if (pending_) {
                return std::format("WIRING: pin {}.{} picked -- click another pin to connect (Esc cancels)",
                                   pending_->first, pending_->second);
            }
            if (selected_) {
                return std::format("SELECTED device {} -- drag the gizmo to move; click a pin to wire",
                                   *selected_);
            }
            return "Click a device to select + move, or click a pin to start a wire";
        }

        // --- MouseListener: drives wake() + click-vs-drag detection ---
        void onMouseDown(int button, const Vector2& pos) override {
            scheduler_.wake();
            if (button == 0) {
                leftDown_ = true;
                downPos_ = pos;
                movedSincePress_ = false;
            }
        }
        void onMouseMove(const Vector2& pos) override {
            if (leftDown_) {
                if (downPos_.distanceTo(pos) > 5.f) movedSincePress_ = true;
                scheduler_.wake();
            }
        }
        void onMouseUp(int button, const Vector2& pos) override {
            scheduler_.wake();
            if (button == 0) {
                if (!movedSincePress_ && !gizmoPressed_) handleClick(pos);
                leftDown_ = false;
                gizmoPressed_ = false;
            }
        }
        void onMouseWheel(const Vector2&) override { scheduler_.wake(); }

    private:
        Vector3 pinWorld(int device, int pin) const {
            /// Devices are translate-only and parented to the scene root, so a
            /// pin's world position is just the body position plus its offset.
            return devices_[device].body->position.clone().add(devices_[device].pins[pin].offset);
        }

        Vector2 toNdc(const Vector2& pixel) const {
            const auto size = canvas_.size();
            return Vector2{pixel.x / size.width() * 2.f - 1.f,
                           -(pixel.y / size.height() * 2.f - 1.f)};
        }

        void handleClick(const Vector2& pixel) {
            raycaster_.setFromCamera(toNdc(pixel), camera_);
            const auto hits = raycaster_.intersectObjects(pickables_, false);
            if (hits.empty()) {
                cancel();
                return;
            }
            Object3D* hit = hits.front().object;
            if (const auto pinIt = pinLookup_.find(hit); pinIt != pinLookup_.end()) {
                startOrFinishWire(pinIt->second.first, pinIt->second.second);
            } else if (const auto bodyIt = bodyLookup_.find(hit); bodyIt != bodyLookup_.end()) {
                selectDevice(bodyIt->second);
            } else {
                cancel();
            }
        }

        void startOrFinishWire(int device, int pin) {
            deselect();// wiring is separate from moving -- drop any gizmo selection
            const std::pair<int, int> here{device, pin};
            if (!pending_) {
                pending_ = here;
            } else if (*pending_ == here) {
                pending_.reset();// clicking the same pin cancels
            } else {
                createWire(pending_->first, pending_->second, device, pin);
                pending_.reset();
            }
            updatePinHighlights();
            scheduler_.wake();
        }

        void createWire(int deviceA, int pinA, int deviceB, int pinB) {
            auto geometry = BufferGeometry::create();
            geometry->setFromPoints(std::vector<Vector3>{pinWorld(deviceA, pinA), pinWorld(deviceB, pinB)});
            auto line = Line::create(geometry, wireMaterial_);
            scene_.add(line);
            wires_.push_back(Wire{deviceA, pinA, deviceB, pinB, line, geometry});
        }

        void selectDevice(int device) {
            pending_.reset();
            updatePinHighlights();
            selected_ = device;
            gizmo_.attach(*devices_[device].body);
            gizmo_.setMode("translate");
            scheduler_.wake();
        }

        void deselect() {
            if (selected_) {
                gizmo_.detach();
                selected_.reset();
            }
        }

        void updatePinHighlights() {
            for (int d{0}; d < static_cast<int>(devices_.size()); ++d) {
                for (int p{0}; p < static_cast<int>(devices_[d].pins.size()); ++p) {
                    const bool isPending = pending_ && pending_->first == d && pending_->second == p;
                    auto& pin = devices_[d].pins[p];
                    pin.material->emissive = isPending ? Color{0xff6a00} : Color{0x000000};
                    pin.marker->scale.setScalar(isPending ? 1.5f : 1.f);
                }
            }
        }

        Scene& scene_;
        PerspectiveCamera& camera_;
        Canvas& canvas_;
        TransformControls& gizmo_;
        OrbitControls& orbit_;
        RenderScheduler& scheduler_;

        Raycaster raycaster_;
        std::shared_ptr<MeshStandardMaterial> bodyMaterial_;
        std::shared_ptr<LineBasicMaterial> wireMaterial_;

        std::vector<Device> devices_;
        std::vector<Wire> wires_;
        std::vector<Object3D*> pickables_;
        std::unordered_map<Object3D*, int> bodyLookup_;
        std::unordered_map<Object3D*, std::pair<int, int>> pinLookup_;

        std::optional<std::pair<int, int>> pending_;
        std::optional<int> selected_;

        bool leftDown_{false};
        bool movedSincePress_{false};
        bool gizmoPressed_{false};
        Vector2 downPos_;
    };

    /// Esc cancels a pending wire / deselects, mirroring the clone's keydown.
    struct CancelKey: KeyListener {
        Editor& editor;
        explicit CancelKey(Editor& e): editor{e} {}
        void onKeyPressed(KeyEvent event) override {
            if (event.key == Key::ESCAPE) editor.cancel();
        }
    };

    Canvas* g_canvas{nullptr};

}// namespace

#ifdef __EMSCRIPTEN__
/// Exposed to the shell: JS calls this on load / window-resize / fullscreen to
/// resize the WebGL drawing buffer to the displayed size. setSize() fires
/// threepp's onWindowResize listener, which updates the camera aspect and the
/// renderer, so the scene re-renders crisp at the real resolution (no upscale).
extern "C" EMSCRIPTEN_KEEPALIVE void spike_resize(int width, int height) {
    if (g_canvas && width > 0 && height > 0) g_canvas->setSize({width, height});
}
#endif

int main() {

    Canvas canvas{"threepp -- esim Simulate tab (select / move / wire)"};
    g_canvas = &canvas;

    /// Pass the API explicitly so createRenderer does not prompt on stdin
    /// (on the web that read becomes a blocking browser prompt() dialog).
    auto renderer = createRenderer(canvas, GraphicsAPI::OpenGL);
    renderer->shadowMap().enabled = true;
    renderer->shadowMap().type = ShadowMap::PFC;// PCF-soft, as in the clone

    PerspectiveCamera camera{45, canvas.aspect(), 0.1f, 100.f};
    camera.position.set(2.8f, 1.6f, 4.6f);
    camera.lookAt(0, 0, 0);

    Scene scene;
    scene.background = Color{0xf3efe4};

    scene.add(HemisphereLight::create(0xffffff, 0x8d8d8d, 1.0f));

    auto sun = DirectionalLight::create(0xffffff, 1.6f);
    sun->position.set(4.f, 7.f, 5.f);
    sun->castShadow = true;
    sun->shadow->mapSize.set(1024, 1024);// match the clone's 1024 map (perf)
    scene.add(sun);

    auto floorMaterial = ShadowMaterial::create();
    floorMaterial->transparent = true;
    floorMaterial->opacity = 0.2f;
    auto floor = Mesh::create(CircleGeometry::create(6.f, 64), floorMaterial);
    floor->rotation.x = -math::PI / 2;
    floor->receiveShadow = true;
    scene.add(floor);

    RenderScheduler scheduler;

    OrbitControls orbitControls{camera, canvas};
    orbitControls.enableDamping = false;// as in the clone

    TransformControls transformControls{camera, canvas};
    scene.addRef(transformControls);

    Editor editor{scene, camera, canvas, transformControls, orbitControls, scheduler};
    editor.build();
    canvas.addMouseListener(editor);

    CancelKey cancelKey{editor};
    canvas.addKeyListener(cancelKey);

    /// "dragging-changed": while the gizmo is grabbed, suspend orbit so the two
    /// controls never fight -- the exact toggle the clone performs. The listeners
    /// are named locals because addEventListener stores a pointer to them.
    LambdaEventListener draggingChanged{[&](Event& event) {
        orbitControls.enabled = !std::any_cast<bool>(event.target);
        scheduler.wake();
    }};
    transformControls.addEventListener("dragging-changed", draggingChanged);

    /// "mouseDown" fires only when a gizmo axis is pressed -- flag it so the
    /// matching click is treated as a gizmo grab, not a scene selection.
    LambdaEventListener gizmoPressed{[&](Event&) { editor.markGizmoPress(); }};
    transformControls.addEventListener("mouseDown", gizmoPressed);

    /// Gizmo moved the device -> a frame is needed (wires follow in refreshWires).
    LambdaEventListener objectChanged{[&](Event&) { scheduler.wake(); }};
    transformControls.addEventListener("objectChange", objectChanged);

    canvas.onWindowResize([&](WindowSize size) {
        camera.aspect = size.aspect();
        camera.updateProjectionMatrix();
        renderer->setSize(size);
        scheduler.wake();
    });

    long renderCount{0};
    double fps{0.0};
    int fpsFrames{0};
    auto fpsWindowStart = std::chrono::steady_clock::now();

    std::cout << std::format("threepp editor booted: {} devices, click to select/move, two-click to wire\n",
                             1000);

    /// ON-DEMAND loop. The body runs only while the main loop is awake; an idle
    /// frame renders nothing and then pauses the loop, so the browser stops
    /// scheduling rAF entirely until the next input wakes it. fps is a true
    /// 1/2-second-window count (like the clone's stats.js), so it reads ~display
    /// refresh while you interact and is not dragged down by post-idle gaps.
    canvas.animate([&] {
        if (orbitControls.update()) scheduler.dirty = true;// applies any pending camera change

        if (scheduler.dirty) {
            editor.refreshWires();
            ++renderCount;
            ++fpsFrames;

            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - fpsWindowStart).count();
            if (elapsed >= 0.5) {
                if (elapsed < 2.0) fps = fpsFrames / elapsed;// skip bogus post-idle windows
                fpsFrames = 0;
                fpsWindowStart = now;
            }

            renderer->render(scene, camera);
            scheduler.dirty = false;

#ifdef __EMSCRIPTEN__
            /// Drive the SAME stats.js panel the clone uses (FPS/MS/MB, click to
            /// switch) once per render. fps is the only on-screen readout.
            /// Bracket notation keeps this safe under --closure 1.
            EM_ASM({ if (window['__stats']) window['__stats']['update'](); });
#endif
            if (renderCount == 1 || renderCount % 60 == 0) {
                std::cout << std::format("renders: {} | {:.0f} fps | {}\n", renderCount, fps, editor.status());
            }
        }

        scheduler.sleepIfIdle();
    });
}
