#include "imageflow_private.h"
#include "codecs.h"

struct flow_job * flow_job_create(flow_c * c)
{

    struct flow_job * job = (struct flow_job *)FLOW_malloc(c, sizeof(struct flow_job));
    if (job == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return NULL;
    }
    static int32_t job_id = 0;
    flow_job_configure_recording(c, job, false, false, false, false, false);
    job->next_graph_version = 0;
    job->next_stable_node_id = 0;
    job->debug_job_id = job_id++;
    job->codecs_head = NULL;
    job->codecs_tail = NULL;
    job->max_calc_flatten_execute_passes = 40;
    return job;
}

bool flow_job_configure_recording(flow_c * c, struct flow_job * job, bool record_graph_versions,
                                  bool record_frame_images, bool render_last_graph, bool render_graph_versions,
                                  bool render_animated_graph)
{
    job->record_frame_images = record_frame_images;
    job->record_graph_versions = record_graph_versions;
    job->render_last_graph = render_last_graph;
    job->render_graph_versions = render_graph_versions && record_graph_versions;
    job->render_animated_graph = render_animated_graph && job->render_graph_versions;
    return true;
}
bool flow_job_destroy(flow_c * c, struct flow_job * job) { return FLOW_destroy(c, job); }

struct flow_io * flow_job_get_io(flow_c * c, struct flow_job * job, int32_t placeholder_id)
{
    struct flow_codec_instance * codec = flow_job_get_codec_instance(c, job, placeholder_id);
    if (codec == NULL) {
        // TODO: no error thrown!
        FLOW_add_to_callstack(c);
        return NULL;
    }
    return codec->io;
}

bool flow_job_get_output_buffer(flow_c * c, struct flow_job * job, int32_t placeholder_id,
                                uint8_t ** out_pointer_to_buffer, size_t * out_length)
{
    struct flow_io * io = flow_job_get_io(c, job, placeholder_id);
    if (io == NULL) {
        FLOW_add_to_callstack(c);
        return false;
    }
    if (!flow_io_get_output_buffer(c, io, out_pointer_to_buffer, out_length)) {
        FLOW_add_to_callstack(c);
        return false;
    }
    return true;
}

bool flow_job_add_io(flow_c * c, struct flow_job * job, struct flow_io * io, int32_t placeholder_id,
                     FLOW_DIRECTION direction)
{
    struct flow_codec_instance * r
        = (struct flow_codec_instance *)FLOW_malloc_owned(c, sizeof(struct flow_codec_instance), job);
    if (r == NULL) {
        FLOW_error(c, flow_status_Out_of_memory);
        return false;
    }
    r->next = NULL;
    r->graph_placeholder_id = placeholder_id;
    r->io = io;
    r->codec_id = 0;
    r->codec_state = NULL;
    r->direction = direction;
    if (job->codecs_head == NULL) {
        job->codecs_head = r;
        job->codecs_tail = r;
    } else {
        job->codecs_tail->next = r;
        job->codecs_tail = r;
    }
    if (direction == FLOW_OUTPUT) {
        return true; // We don't determine output codecs this early.
    }

    uint8_t buffer[8];
    int64_t bytes_read = io->read_func(c, io, &buffer[0], 8);
    if (bytes_read != 8) {
        FLOW_error_msg(c, flow_status_IO_error, "Failed to read first 8 bytes of file");
        return false;
    }
    if (!io->seek_function(c, io, 0)) {
        FLOW_error_msg(c, flow_status_IO_error, "Failed to seek to byte 0 in file");
        return false;
    }

    int64_t ctype = flow_codec_select(c, &buffer[0], bytes_read);
    if (ctype == flow_codec_type_null) {
        // unknown
        FLOW_error_msg(c, flow_status_Not_implemented,
                       "Unrecognized leading byte sequence %02x%02x%02x%02x%02x%02x%02x%02x", buffer[0], buffer[1],
                       buffer[2], buffer[3], buffer[4], buffer[5], buffer[6],
                       buffer[7]); // Or bad buffer, unsupported file type, etc.
        return false;
    }
    r->codec_id = ctype;
    if (!flow_codec_initialize(c, r)) {
        FLOW_add_to_callstack(c);
        return false;
    }
    return true;
}

struct flow_codec_instance * flow_job_get_codec_instance(flow_c * c, struct flow_job * job, int32_t by_placeholder_id)
{
    struct flow_codec_instance * current = job->codecs_head;
    while (current != NULL) {
        if (current->graph_placeholder_id == by_placeholder_id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}


bool flow_job_decoder_set_downscale_hints_by_placeholder_id(flow_c * c, struct flow_job * job, int32_t placeholder_id,
                                                            int64_t if_wider_than, int64_t or_taller_than,
                                                            int64_t downscaled_min_width, int64_t downscaled_min_height,
                                                            bool scale_luma_spatially,
                                                            bool gamma_correct_for_srgb_during_spatial_luma_scaling)
{
    struct flow_decoder_downscale_hints hints;
    hints.or_if_taller_than = or_taller_than;
    hints.downscale_if_wider_than = if_wider_than;
    hints.downscaled_min_height = downscaled_min_height;
    hints.downscaled_min_width = downscaled_min_width;
    hints.scale_luma_spatially = scale_luma_spatially;
    hints.gamma_correct_for_srgb_during_spatial_luma_scaling = gamma_correct_for_srgb_during_spatial_luma_scaling;

    struct flow_codec_instance * codec = flow_job_get_codec_instance(c, job, placeholder_id);
    if (codec == NULL) {
        FLOW_error(c, flow_status_Invalid_argument);
        return false;
    }
    if (!flow_codec_decoder_set_downscale_hints(c, codec, &hints, false)) {
        FLOW_error_return(c);
    }
    return true;
}

bool flow_job_get_decoder_info(flow_c * c, struct flow_job * job, int32_t by_placeholder_id,
                               struct flow_decoder_info * info)
{
    struct flow_codec_instance * current = flow_job_get_codec_instance(c, job, by_placeholder_id);
    if (current == NULL) {
        FLOW_error(c, flow_status_Invalid_argument); // Bad placeholder id
        return false;
    }
    if (current->direction != FLOW_INPUT) {
        FLOW_error(c, flow_status_Invalid_argument); // Bad placeholder id
        return false;
    }
    if (!flow_codec_decoder_get_info(c, current->codec_state, current->codec_id, info)) {
        FLOW_error_return(c);
    }
    return true;
}
