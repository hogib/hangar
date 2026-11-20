// SPDX-License-Identifier: GPL-3.0-or-later
/* A weights file (lib/nn: nn_hgw_parse, nn_graph_build and the loader's checks
 * 2 to 6 in nn_load_mem, then nn_step on what loaded). The policy module loads
 * the file it is told to by a load_weights call: it comes from the operator's
 * training machine, over the tether, onto the vehicle. Check 1 (the file's
 * BLAKE2s against the hash the call names) is skipped here, as a fuzzer cannot
 * meet it; what is tested is what a file with the right hash but the wrong
 * contents (a bad exporter, a damaged copy hashed after the damage) can do.
 *
 * Input: the bytes of a .hgw file. The architecture hash in its header is
 * rewritten to match its graph before the load, for the same reason. */
#include "fuzz.h"

#include "nn_graph.h"
#include "nn_hgw.h"

#define MAX_IO 64
#define IO_CAP (1u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* 8-byte aligned and exactly as long as the file, as the loader's own
	 * copy is (nn_hgw_map): tensors are read in place. */
	void *file = NULL;
	if (posix_memalign(&file, 64, size ? size : 1) != 0)
		abort();
	memcpy(file, data, size);

	nn_hgw_file f;
	char reason[160] = "";
	if (nn_hgw_parse(file, size, &f, reason, sizeof reason) == 0) {
		FZ_NOTE("parsed: %u inputs, %u outputs, %u probes", f.n_inputs, f.n_outputs, f.n_probes);
		/* The header carries the hash of the architecture it describes
		 * (check 2): written here as the loader will compute it, or no
		 * mutated graph would get past that comparison. */
		uint64_t arch = nn_hgw_arch_hash(&f);
		memcpy((unsigned char *)file + ((const unsigned char *)&f.hdr->arch_hash - (const unsigned char *)file), &arch,
		       sizeof arch);
	} else {
		FZ_NOTE("parse: %s", reason);
		FZ_CHECK(strnlen(reason, sizeof reason) < sizeof reason);
	}

	nn_load_req req;
	memset(&req, 0, sizeof req);
	nn_report rep;
	memset(&rep, 0, sizeof rep);
	struct nn_model *m = NULL;
	int rc = nn_load_mem(file, size, &req, &m, &rep);
	FZ_NOTE("load: %d, check %d (%s): %s", rc, rep.check, rep.name, rep.reason);
	FZ_CHECK(strnlen(rep.reason, sizeof rep.reason) < sizeof rep.reason);
	FZ_CHECK((rc == 0) == (m != NULL));
	if (m) {
		/* A few ticks on inputs of zeros and of ones, into buffers of exactly
		 * the sizes the model says. */
		void *in[MAX_IO];
		float *out[MAX_IO];
		int ok = m->n_inputs <= MAX_IO && m->n_outputs <= MAX_IO;
		for (uint32_t i = 0; ok && i < m->n_inputs; i++)
			ok = nn_input_bytes(m, i) <= IO_CAP;
		for (uint32_t j = 0; ok && j < m->n_outputs; j++)
			ok = nn_output_count(m, j) <= IO_CAP / 4;
		if (ok) {
			for (uint32_t i = 0; i < m->n_inputs; i++)
				in[i] = calloc(1, nn_input_bytes(m, i) + 1);
			for (uint32_t j = 0; j < m->n_outputs; j++)
				out[j] = calloc(nn_output_count(m, j) + 1, sizeof(float));
			for (int tick = 0; tick < 4; tick++) {
				if (tick == 2)
					for (uint32_t i = 0; i < m->n_inputs; i++)
						memset(in[i], 0x3f, nn_input_bytes(m, i));
				(void)nn_step(m, (const void *const *)in, out);
			}
			nn_reset(m);
			for (uint32_t i = 0; i < m->n_inputs; i++)
				free(in[i]);
			for (uint32_t j = 0; j < m->n_outputs; j++)
				free(out[j]);
		}
		nn_free(m);
	}
	free(file);
	return 0;
}
