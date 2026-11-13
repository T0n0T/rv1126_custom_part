/*
 * RKNN board smoke test for the flow_demo helmet models.
 *
 * This is deliberately a runtime-contract test.  It does not decode YOLO
 * outputs or claim detector accuracy.  It verifies model loading, tensor
 * attributes, repeated inference and output ownership on the target board.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "rknn_api.h"

#define DEFAULT_RUNS 5U
#define MAX_PATH 4096U

struct options {
	const char *model_path;
	const char *input_path;
	unsigned int runs;
	int want_float;
	const char *dump_dir;
};

static const char *tensor_type_name(rknn_tensor_type type)
{
	switch (type) {
	case RKNN_TENSOR_FLOAT32:
		return "float32";
	case RKNN_TENSOR_FLOAT16:
		return "float16";
	case RKNN_TENSOR_INT8:
		return "int8";
	case RKNN_TENSOR_UINT8:
		return "uint8";
	case RKNN_TENSOR_INT16:
		return "int16";
	case RKNN_TENSOR_UINT16:
		return "uint16";
	case RKNN_TENSOR_INT32:
		return "int32";
	case RKNN_TENSOR_UINT32:
		return "uint32";
	default:
		return "unknown";
	}
}

static const char *tensor_format_name(rknn_tensor_format format)
{
	switch (format) {
	case RKNN_TENSOR_NCHW:
		return "NCHW";
	case RKNN_TENSOR_NHWC:
		return "NHWC";
	case RKNN_TENSOR_NC1HWC2:
		return "NC1HWC2";
	case RKNN_TENSOR_UNDEFINED:
		return "UNDEFINED";
	default:
		return "unknown";
	}
}

static const char *tensor_qnt_name(rknn_tensor_qnt_type type)
{
	switch (type) {
	case RKNN_TENSOR_QNT_NONE:
		return "none";
	case RKNN_TENSOR_QNT_DFP:
		return "dfp";
	case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
		return "affine";
	default:
		return "unknown";
	}
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int parse_uint(const char *value, unsigned int *result)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
	    parsed > UINT32_MAX)
		return -1;
	*result = (unsigned int)parsed;
	return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
	FILE *file;
	long length;
	uint8_t *buffer;
	size_t read_size;

	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET) != 0 || length == 0) {
		fprintf(stderr, "cannot size %s\n", path);
		fclose(file);
		return -1;
	}
	buffer = (uint8_t *)malloc((size_t)length);
	if (!buffer) {
		fprintf(stderr, "out of memory reading %s\n", path);
		fclose(file);
		return -1;
	}
	read_size = fread(buffer, 1, (size_t)length, file);
	fclose(file);
	if (read_size != (size_t)length) {
		fprintf(stderr, "short read for %s: %zu/%ld\n", path, read_size, length);
		free(buffer);
		return -1;
	}
	*data = buffer;
	*size = read_size;
	return 0;
}

static void print_attr(const char *kind, const rknn_tensor_attr *attr)
{
	uint32_t i;

	printf("%s[%u] name=%s type=%s fmt=%s qnt=%s dims=",
	       kind, attr->index, attr->name, tensor_type_name(attr->type),
	       tensor_format_name(attr->fmt), tensor_qnt_name(attr->qnt_type));
	for (i = 0; i < attr->n_dims; ++i)
		printf("%s%u", i == 0 ? "" : "x", attr->dims[i]);
	printf(" elems=%u bytes=%u zp=%" PRId32 " scale=%.9g\n", attr->n_elems,
	       attr->size, attr->zp, attr->scale);
}

static void usage(const char *program)
{
	printf("usage: %s --model MODEL.rknn --input IMAGE.rgb [options]\n", program);
	printf("  --model PATH       RKNN model\n");
	printf("  --input PATH       raw 640x640 RGB bytes\n");
	printf("  --runs N           repeated inferences (default: %u)\n", DEFAULT_RUNS);
	printf("  --want-float 0|1   request float output (default: 1)\n");
	printf("  --dump-dir DIR     write the last output tensors as output-N.bin\n");
}

static int parse_options(int argc, char **argv, struct options *options)
{
	static const struct option long_options[] = {
		{"model", required_argument, NULL, 'm'},
		{"input", required_argument, NULL, 'i'},
		{"runs", required_argument, NULL, 'r'},
		{"want-float", required_argument, NULL, 'f'},
		{"dump-dir", required_argument, NULL, 'd'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int option;

	options->runs = DEFAULT_RUNS;
	options->want_float = 1;
	while ((option = getopt_long(argc, argv, "m:i:r:f:d:h", long_options, NULL)) != -1) {
		switch (option) {
		case 'm':
			options->model_path = optarg;
			break;
		case 'i':
			options->input_path = optarg;
			break;
		case 'r':
			if (parse_uint(optarg, &options->runs) != 0)
				return -1;
			break;
		case 'f':
			if (!strcmp(optarg, "0"))
				options->want_float = 0;
			else if (strcmp(optarg, "1"))
				return -1;
			break;
		case 'd':
			options->dump_dir = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			return -1;
		}
	}
	return options->model_path && options->input_path ? 0 : -1;
}

static int dump_outputs(const char *directory, const rknn_output *outputs,
			uint32_t count)
{
	uint32_t i;
	char path[MAX_PATH];
	FILE *file;

	if (!directory)
		return 0;
	if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "cannot create dump directory %s: %s\n", directory,
			strerror(errno));
		return -1;
	}
	for (i = 0; i < count; ++i) {
		int written = snprintf(path, sizeof(path), "%s/output-%u.bin", directory, i);
		if (written < 0 || (size_t)written >= sizeof(path)) {
			fprintf(stderr, "dump path is too long for output %u\n", i);
			return -1;
		}
		file = fopen(path, "wb");
		if (!file) {
			fprintf(stderr, "cannot open dump %s: %s\n", path, strerror(errno));
			return -1;
		}
		if (outputs[i].size > 0 &&
		    fwrite(outputs[i].buf, 1, outputs[i].size, file) != outputs[i].size) {
			fprintf(stderr, "short write for dump %s\n", path);
			fclose(file);
			return -1;
		}
		fclose(file);
	}
	return 0;
}

static int query_attr(rknn_context context, rknn_query_cmd command,
			      uint32_t index, rknn_tensor_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->index = index;
	return rknn_query(context, command, attr, sizeof(*attr));
}

int main(int argc, char **argv)
{
	struct options options = {0};
	uint8_t *model = NULL;
	uint8_t *input_data = NULL;
	size_t model_size = 0;
	size_t input_size = 0;
	rknn_context context = 0;
	rknn_input_output_num io_num;
	rknn_tensor_attr input_attr;
	rknn_tensor_attr *output_attrs = NULL;
	rknn_input input;
	rknn_output *outputs = NULL;
	rknn_sdk_version sdk_version;
	uint32_t i;
	uint64_t total_ns = 0;
	int ret;

	if (parse_options(argc, argv, &options) != 0) {
		usage(argv[0]);
		return 2;
	}
	if (read_file(options.model_path, &model, &model_size) != 0 ||
	    read_file(options.input_path, &input_data, &input_size) != 0)
		goto fail;

	ret = rknn_init(&context, model, (uint32_t)model_size, 0, NULL);
	if (ret < 0) {
		fprintf(stderr, "rknn_init failed: %d\n", ret);
		goto fail;
	}
	memset(&sdk_version, 0, sizeof(sdk_version));
	ret = rknn_query(context, RKNN_QUERY_SDK_VERSION, &sdk_version,
				 sizeof(sdk_version));
	if (ret == 0)
		printf("sdk api=%s driver=%s\n", sdk_version.api_version,
		       sdk_version.drv_version);
	else
		printf("sdk query failed: %d\n", ret);

	memset(&io_num, 0, sizeof(io_num));
	if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != 0 ||
	    io_num.n_input != 1 || io_num.n_output == 0) {
		fprintf(stderr, "unsupported io count: inputs=%u outputs=%u\n",
			io_num.n_input, io_num.n_output);
		goto fail_context;
	}
	if (query_attr(context, RKNN_QUERY_INPUT_ATTR, 0, &input_attr) != 0) {
		fprintf(stderr, "input attribute query failed\n");
		goto fail_context;
	}
	print_attr("input", &input_attr);
	if (input_attr.n_elems == 0 || input_attr.n_elems != input_size) {
		fprintf(stderr, "input size mismatch: model expects %u elements, file has %zu bytes\n",
			input_attr.n_elems, input_size);
		goto fail_context;
	}

	output_attrs = calloc(io_num.n_output, sizeof(*output_attrs));
	outputs = calloc(io_num.n_output, sizeof(*outputs));
	if (!output_attrs || !outputs)
		goto fail_context;
	for (i = 0; i < io_num.n_output; ++i) {
		if (query_attr(context, RKNN_QUERY_OUTPUT_ATTR, i, &output_attrs[i]) != 0) {
			fprintf(stderr, "output attribute query failed for %u\n", i);
			goto fail_context;
		}
		print_attr("output", &output_attrs[i]);
		outputs[i].index = i;
		outputs[i].want_float = (uint8_t)options.want_float;
	}

	memset(&input, 0, sizeof(input));
	input.index = 0;
	input.buf = input_data;
	input.size = (uint32_t)input_size;
	input.pass_through = 0;
	input.type = RKNN_TENSOR_UINT8;
	input.fmt = RKNN_TENSOR_NHWC;
	printf("input_runtime type=%s fmt=%s pass_through=%u bytes=%u\n",
	       tensor_type_name(input.type), tensor_format_name(input.fmt),
	       input.pass_through, input.size);

	for (i = 0; i < options.runs; ++i) {
		uint64_t start = monotonic_ns();
		uint64_t elapsed_ns;
		if (rknn_inputs_set(context, 1, &input) != 0 ||
		    rknn_run(context, NULL) != 0 ||
		    rknn_outputs_get(context, io_num.n_output, outputs, NULL) != 0) {
			fprintf(stderr, "inference failed at run %u\n", i);
			goto fail_context;
		}
		elapsed_ns = monotonic_ns() - start;
		total_ns += elapsed_ns;
		printf("run=%u elapsed_us=%" PRIu64, i,
		       elapsed_ns / UINT64_C(1000));
		for (uint32_t j = 0; j < io_num.n_output; ++j) {
			if (options.want_float && outputs[j].buf && outputs[j].size >= sizeof(float))
				printf(" output%u_bytes=%u output%u_first=%.7g", j,
				       outputs[j].size, j, ((const float *)outputs[j].buf)[0]);
			else
				printf(" output%u_bytes=%u", j, outputs[j].size);
		}
		printf("\n");
		if (dump_outputs(options.dump_dir, outputs, io_num.n_output) != 0)
			goto fail_context;
		if (rknn_outputs_release(context, io_num.n_output, outputs) != 0) {
			fprintf(stderr, "output release failed at run %u\n", i);
			goto fail_context;
		}
	}
	printf("RKNN_SMOKE_OK runs=%u avg_elapsed_us=%" PRIu64 " outputs=%u\n",
	       options.runs, total_ns / UINT64_C(1000) / options.runs, io_num.n_output);
	free(outputs);
	free(output_attrs);
	rknn_destroy(context);
	free(input_data);
	free(model);
	return 0;

fail_context:
	if (context)
		rknn_destroy(context);
	free(outputs);
	free(output_attrs);
	free(input_data);
	free(model);
	return 1;
fail:
	free(input_data);
	free(model);
	return 1;
}
