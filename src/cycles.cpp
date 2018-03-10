/* Goxel 3D voxels editor
 *
 * copyright (c) 2018 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "device/device.h"
#include "render/camera.h"
#include "render/film.h"
#include "render/graph.h"
#include "render/light.h"
#include "render/mesh.h"
#include "render/nodes.h"
#include "render/object.h"
#include "render/session.h"
#include "render/shader.h"
#include "util/util_transform.h"
#include "util/util_foreach.h"

#include <memory> // for make_unique

extern "C" {
#include "goxel.h"
}

// Convenience macro for cycles string creation.
#define S(v) ccl::ustring(v)

static ccl::Session *g_session = NULL;
static ccl::BufferParams g_buffer_params;
static ccl::SessionParams g_session_params;

static ccl::Shader *create_cube_shader(void)
{
    ccl::Shader *shader = new ccl::Shader();
    shader->name = "cubeShader";
    ccl::ShaderGraph *shaderGraph = new ccl::ShaderGraph();

    const ccl::NodeType *colorNodeType = ccl::NodeType::find(S("attribute"));
    ccl::ShaderNode *colorShaderNode = static_cast<ccl::ShaderNode*>(
            colorNodeType->create(colorNodeType));
    colorShaderNode->name = "colorNode";
    colorShaderNode->set(*colorShaderNode->type->find_input(S("attribute")),
                         S("Col"));
    shaderGraph->add(colorShaderNode);

    const ccl::NodeType *diffuseBSDFNodeType = ccl::NodeType::find(S("diffuse_bsdf"));
    ccl::ShaderNode *diffuseBSDFShaderNode = static_cast<ccl::ShaderNode*>(
            diffuseBSDFNodeType->create(diffuseBSDFNodeType));
    diffuseBSDFShaderNode->name = "diffuseBSDFNode";
    shaderGraph->add(diffuseBSDFShaderNode);

    shaderGraph->connect(
        colorShaderNode->output("Color"),
        diffuseBSDFShaderNode->input("Color")
    );
    shaderGraph->connect(
        diffuseBSDFShaderNode->output("BSDF"),
        shaderGraph->output()->input("Surface")
    );

    shader->set_graph(shaderGraph);
    return shader;
}

static ccl::Shader *create_light_shader(void)
{
    ccl::Shader *shader = new ccl::Shader();
    shader->name = "lightShader";
    ccl::ShaderGraph *shaderGraph = new ccl::ShaderGraph();

    const ccl::NodeType *emissionNodeType = ccl::NodeType::find(S("emission"));
    ccl::ShaderNode *emissionShaderNode = static_cast<ccl::ShaderNode*>(
            emissionNodeType->create(emissionNodeType));
    emissionShaderNode->name = "emissionNode";
    emissionShaderNode->set(
        *emissionShaderNode->type->find_input(S("strength")),
        1.0f
    );
    emissionShaderNode->set(
        *emissionShaderNode->type->find_input(S("color")),
        ccl::make_float3(1.0, 1.0, 1.0)
    );

    shaderGraph->add(emissionShaderNode);

    shaderGraph->connect(
        emissionShaderNode->output("Emission"),
        shaderGraph->output()->input("Surface")
    );

    shader->set_graph(shaderGraph);
    return shader;
}

static ccl::Mesh *create_mesh_for_block(
        const mesh_t *mesh, const int block_pos[3])
{
    ccl::Mesh *ret = NULL;
    int nb = 0, i, j;
    voxel_vertex_t* vertices;
    ccl::Attribute *attr;

    ret = new ccl::Mesh();
    ret->subdivision_type = ccl::Mesh::SUBDIVISION_NONE;

    vertices = (voxel_vertex_t*)calloc(
            BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE * 6 * 4, sizeof(*vertices));
    nb = mesh_generate_vertices(mesh, block_pos, 0, vertices);
    if (!nb) goto end;

    ret->reserve_mesh(nb * 4, nb * 2);
    for (i = 0; i < nb; i++) { // Once per quad.
        for (j = 0; j < 4; j++) {
            ret->add_vertex(ccl::make_float3(
                        vertices[i * 4 + j].pos[0],
                        vertices[i * 4 + j].pos[1],
                        vertices[i * 4 + j].pos[2]));
        }
        ret->add_triangle(i * 4 + 0, i * 4 + 1, i * 4 + 2, 0, false);
        ret->add_triangle(i * 4 + 2, i * 4 + 3, i * 4 + 0, 0, false);
    }

    // Set color attribute.
    attr = ret->attributes.add(S("Col"), ccl::TypeDesc::TypeColor,
            ccl::ATTR_ELEMENT_CORNER_BYTE);
    for (i = 0; i < nb * 6; i++) {
        attr->data_uchar4()[i] = ccl::make_uchar4(
                vertices[i / 6 * 4].color[0],
                vertices[i / 6 * 4].color[1],
                vertices[i / 6 * 4].color[2],
                vertices[i / 6 * 4].color[3]
        );
    }

end:
    free(vertices);
    return ret;
}

static void get_light_dir(float out[3])
{
    const renderer_t *rend = &goxel->rend;
    float m[4][4], light_dir[4];
    const float z[4] = {0, 0, 1, 0};

    mat4_set_identity(m);
    mat4_irotate(m, rend->light.yaw, 0, 0, 1);
    mat4_irotate(m, rend->light.pitch, 1, 0, 0);
    mat4_mul_vec4(m, z, light_dir);
    vec3_mul(light_dir, -1, out);
}

static ccl::Scene *create_scene(int w, int h)
{
    mesh_t *gmesh = goxel->render_mesh;
    int block_pos[3];
    float light_dir[3];
    mesh_iterator_t iter;
    ccl::Scene *scene;
    ccl::SceneParams scene_params;
    scene_params.shadingsystem = ccl::SHADINGSYSTEM_OSL;
    // scene_params.shadingsystem = ccl::SHADINGSYSTEM_SVM;

    scene = new ccl::Scene(scene_params, g_session->device);
    scene->camera->width = w;
    scene->camera->height = h;
    scene->camera->fov = 20.0 * DD2R;
    scene->camera->type = ccl::CameraType::CAMERA_PERSPECTIVE;
    scene->camera->full_width = scene->camera->width;
    scene->camera->full_height = scene->camera->height;
    scene->film->exposure = 1.0f;

    // Set camera.
    // XXX: cleanup!
    float mat[4][4];
    float rot[4];
    assert(sizeof(scene->camera->matrix) == sizeof(mat));
    mat4_set_identity(mat);
    mat4_itranslate(mat, -goxel->camera.ofs[0],
                         -goxel->camera.ofs[1],
                         -goxel->camera.ofs[2]);
    quat_copy(goxel->camera.rot, rot);
    rot[0] *= -1;
    mat4_imul_quat(mat, rot);
    mat4_itranslate(mat, 0, 0, goxel->camera.dist);
    mat4_iscale(mat, 1, 1, -1);
    mat4_transpose(mat, mat);
    memcpy(&scene->camera->matrix, mat, sizeof(mat));

    ccl::Shader *object_shader = create_cube_shader();
    object_shader->tag_update(scene);
    scene->shaders.push_back(object_shader);

    iter = mesh_get_iterator(gmesh,
            MESH_ITER_BLOCKS | MESH_ITER_INCLUDES_NEIGHBORS);
    while (mesh_iter(&iter, block_pos)) {
        ccl::Mesh *mesh = create_mesh_for_block(gmesh, block_pos);
        mesh->used_shaders.push_back(object_shader);
        scene->meshes.push_back(mesh);
        ccl::Object *object = new ccl::Object();
        object->name = "mesh";
        object->mesh = mesh;
        object->tfm = ccl::transform_identity() *
            ccl::transform_translate(ccl::make_float3(
                    block_pos[0], block_pos[1], block_pos[2]));
        scene->objects.push_back(object);
    }

    ccl::Light *light = new ccl::Light();
    /*
    foreach(const ccl::SocketType& socket, ((ccl::Node*)light)->type->inputs) {
        LOG_D("XXX %s", socket.name.c_str());
    }
    */

    light->type = ccl::LIGHT_DISTANT;
    light->size = 0.05f;
    get_light_dir(light_dir);
    light->dir = ccl::make_float3(light_dir[0], light_dir[1], light_dir[2]);
    light->tag_update(scene);

    ccl::Shader *light_shader = create_light_shader();
    light_shader->tag_update(scene);
    scene->shaders.push_back(light_shader);
    light->shader = light_shader;
    scene->lights.push_back(light);

    scene->camera->compute_auto_viewplane();
    scene->camera->need_update = true;
    scene->camera->need_device_update = true;
    return scene;
}

void cycles_init(void)
{
    ccl::DeviceType device_type;
    ccl::DeviceInfo device_info;
    ccl::vector<ccl::DeviceInfo>& devices = ccl::Device::available_devices();

    device_type = ccl::Device::type_from_string("CPU");
    for (const ccl::DeviceInfo& device : devices) {
        if (device_type == device.type) {
            device_info = device;
            break;
        }
    }
    g_session_params.progressive = true;
    g_session_params.start_resolution = 64;
    g_session_params.device = device_info;
    g_session_params.samples = 20;
    // session_params.threads = 1;
}

/*
 * Compute a value that should change when we need to rerender the scene.
 */
static uint64_t get_render_key(void)
{
    uint64_t key;
    const camera_t *camera = &goxel->camera;
    key = mesh_get_key(goxel->render_mesh);
    key = crc64(key, (uint8_t*)camera->view_mat, sizeof(camera->view_mat));
    key = crc64(key, (uint8_t*)camera->proj_mat, sizeof(camera->proj_mat));
    return key;
}

void cycles_render(const int rect[4])
{
    static ccl::DeviceDrawParams draw_params = ccl::DeviceDrawParams();
    static uint64_t last_key = 0;
    uint64_t key;

    int w = rect[2];
    int h = rect[3];

    g_buffer_params.width = w;
    g_buffer_params.height = h;
    g_buffer_params.full_width = w;
    g_buffer_params.full_height = h;

    GL(glViewport(rect[0], rect[1], rect[2], rect[3]));
    GL(glMatrixMode(GL_PROJECTION));
    GL(glLoadIdentity());

    GL(glOrtho(0, g_buffer_params.width, 0, g_buffer_params.height, -1, 1));
    GL(glMatrixMode(GL_MODELVIEW));
    GL(glLoadIdentity());
    GL(glUseProgram(0));

    key = get_render_key();
    if (key != last_key) {
        last_key = key;
        if (g_session) delete g_session;
        g_session = new ccl::Session(g_session_params);
        g_session->scene = create_scene(w, h);
        g_session->reset(g_buffer_params, g_session_params.samples);
        g_session->start();
    }

    if (!g_session) return;

    g_session->draw(g_buffer_params, draw_params);

    std::string status;
    std::string substatus;
    g_session->progress.get_status(status, substatus);
}