#pragma once

#include <wayfire/object.hpp>
#include <wayfire/output.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace wf
{

/**
 * When the workspace wall is rendered via a render hook, the frame event
 * is emitted on each frame.
 *
 * The target framebuffer is passed as signal data.
 */
struct wall_frame_event_t : public signal_data_t
{
    const wf::framebuffer_t& target;
    wall_frame_event_t(const wf::framebuffer_t& t) : target(t) {}
};

/**
 * A helper class to render workspaces arranged in a grid.
 */
class workspace_wall_t : public wf::signal_provider_t
{
  public:
    /**
     * Create a new workspace wall on the given output.
     */
    workspace_wall_t(wf::output_t *_output) : output(_output)
    {
        this->viewport = get_wall_rectangle();

        auto wsize = this->output->workspace->get_workspace_grid_size();
        this->streams.resize(wsize.width);
        for (int i = 0; i < wsize.width; i++)
        {
            this->streams[i].resize(wsize.height);
            for (int j = 0; j < wsize.height; j++)
                this->streams[i][j].ws = {i, j};
        }
    }

    ~workspace_wall_t()
    {
        stop_output_renderer();

        OpenGL::render_begin();
        for (auto& row : this->streams)
        {
            for (auto& stream : row)
                stream.buffer.release();
        }
        OpenGL::render_end();
    }

    /**
     * Set the color of the background outside of workspaces.
     *
     * @param color The new background color.
     */
    void set_background_color(const wf::color_t& color)
    {
        this->background_color = color;
    }

    /**
     * Set the size of the gap between adjacent workspaces, both horizontally
     * and vertically.
     *
     * @param size The new gap size, in pixels.
     */
    void set_gap_size(int size)
    {
        this->gap_size = size;
    }

    /**
     * Set which part of the workspace wall to render.
     *
     * If the output has effective resolution WxH and the gap size is G, then a
     * workspace with coordinates (i, j) has geometry
     * {i * (W + G), j * (H + G), W, H}.
     *
     * All other regions are painted with the background color.
     *
     * @param viewport_geometry The part of the workspace wall to render.
     */
    void set_viewport(const wf::geometry_t& viewport_geometry)
    {
        /*
         * XXX: Check which workspaces should be stopped.
         * Algorithm can be reduced to O(N) but O(N^2) should be more than enough.
         */
        auto previously_visible = get_visible_workspaces(this->viewport);
        auto newly_visible = get_visible_workspaces(viewport_geometry);
        for (wf::point_t old : previously_visible)
        {
            auto it = std::find_if(newly_visible.begin(), newly_visible.end(),
                [&] (auto neww) { return neww == old; });
            if (it == newly_visible.end())
                output->render->workspace_stream_stop(streams[old.x][old.y]);
        }

        this->viewport = viewport_geometry;
    }

    /**
     * Render the selected viewport on the framebuffer.
     *
     * @param fb The framebuffer to render on.
     * @param geometry The rectangle in fb to draw to, in the same coordinate
     *   system as the framebuffer's geometry.
     */
    void render_wall(const wf::framebuffer_t& fb, wf::geometry_t geometry)
    {
        update_streams();

        OpenGL::render_begin(fb);
        fb.logic_scissor(geometry);

        OpenGL::clear(this->background_color);
        auto wall_matrix =
            calculate_viewport_transformation_matrix(this->viewport, geometry);
        /* After all transformations of the framebuffer, the workspace should
         * span the visible part of the OpenGL coordinate space. */
        const wf::geometry_t workspace_geometry = { -1, 1, 2, -2 };
        for (auto& ws : get_visible_workspaces(this->viewport))
        {
            auto ws_matrix = calculate_workspace_matrix(ws);
            OpenGL::render_transformed_texture(
                this->streams[ws.x][ws.y].buffer.tex, workspace_geometry,
                fb.get_orthographic_projection() * wall_matrix * ws_matrix);
        }

        OpenGL::render_end();

        wall_frame_event_t data{fb};
        this->emit_signal("frame", &data);
    }

    /**
     * Register a render hook and paint the whole output as a desktop wall
     * with the set parameters.
     */
    void start_output_renderer()
    {
        if (!render_hook_set)
        {
            this->output->render->set_renderer(on_render);
            render_hook_set = true;
        }
    }

    /**
     * Stop repainting the whole output.
     */
    void stop_output_renderer()
    {
        if (render_hook_set)
        {
            this->output->render->set_renderer(nullptr);
            render_hook_set = false;
        }
    }

    /**
     * Calculate the geometry of a particular workspace, as described in
     * set_viewport().
     *
     * @param ws The workspace whose geometry is to be computed.
     */
    wf::geometry_t get_workspace_rectangle(const wf::point_t& ws) const
    {
        auto size = this->output->get_screen_size();
        return {
            ws.x * (size.width + gap_size),
            ws.y * (size.height + gap_size),
            size.width,
            size.height
        };
    }

    /**
     * Calculate the whole workspace wall region, including gaps around it.
     */
    wf::geometry_t get_wall_rectangle() const
    {
        auto size = this->output->get_screen_size();
        auto workspace_size = this->output->workspace->get_workspace_grid_size();

        return {
            -gap_size,
            -gap_size,
            workspace_size.width * (size.width + gap_size) + gap_size,
            workspace_size.height * (size.height + gap_size) + gap_size
        };
    }

  protected:
    wf::output_t *output;

    wf::color_t background_color = {0, 0, 0, 0};
    int gap_size = 0;

    wf::geometry_t viewport = {0, 0, 0, 0};
    std::vector<std::vector<wf::workspace_stream_t>> streams;
    /** Update or start visible streams */
    void update_streams()
    {
        for (auto& ws : get_visible_workspaces(viewport))
        {
            auto& stream = streams[ws.x][ws.y];
            if (stream.running) {
                output->render->workspace_stream_update(stream);
            } else {
                output->render->workspace_stream_start(stream);
            }
        }
    }

    /**
     * Get a list of workspaces visible in the viewport.
     */
    std::vector<wf::point_t> get_visible_workspaces(wf::geometry_t viewport) const
    {
        std::vector<wf::point_t> visible;
        auto wsize = output->workspace->get_workspace_grid_size();
        for (int i = 0; i < wsize.width; i++)
        {
            for (int j =0; j < wsize.height; j++)
            {
                if (viewport & get_workspace_rectangle({i, j}))
                    visible.push_back({i, j});
            }
        }

        return visible;
    }

    /**
     * Calculate the workspace matrix.
     *
     * Workspaces are always rendered with width/height 2 and centered around (0, 0).
     * To obtain the correct output image, the following is done:
     *
     * 1. Output rotation is undone from the workspace stream texture.
     * 2. Workspace quad is scaled to the correct size.
     * 3. Workspace quad is translated to the correct global position.
     */
    glm::mat4 calculate_workspace_matrix(const wf::point_t& ws) const
    {
        auto target_geometry = get_workspace_rectangle(ws);
        auto fb = output->render->get_target_framebuffer();
        auto translation = glm::translate(glm::mat4(1.0),
            glm::vec3{target_geometry.x, target_geometry.y, 0.0});
        return translation * glm::inverse(fb.get_orthographic_projection());
    }

    /**
     * Calculate the viewport transformation matrix.
     *
     * This matrix transforms the workspace's quad from the logical wall space
     * to the actual box to be displayed on the screen.
     */
    glm::mat4 calculate_viewport_transformation_matrix(
        const wf::geometry_t& viewport, const wf::geometry_t& target) const
    {
        const double scale_x = target.width * 1.0 / viewport.width;
        const double scale_y = target.height * 1.0 / viewport.height;

        const double x_after_scale = viewport.x * scale_x;
        const double y_after_scale = viewport.y * scale_y;

        auto scaling = glm::scale(glm::mat4(1.0), glm::vec3{scale_x, scale_y, 1.0});
        auto translation = glm::translate(glm::mat4(1.0),
            glm::vec3{target.x - x_after_scale, target.y - y_after_scale, 0.0});
        return translation * scaling;
    }

    bool render_hook_set = false;
    wf::render_hook_t on_render = [=] (const wf::framebuffer_t& target)
    {
        render_wall(target, this->output->get_relative_geometry());
    };
};
}
