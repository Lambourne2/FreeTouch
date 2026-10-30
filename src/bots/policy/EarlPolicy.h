#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The neural network behind Necto and Nexto (RLGym's EARLPerceiver), run on the CPU in plain C++: no Python, no
// PyTorch, no ONNX Runtime in game. Weights come from tools/export_policy.py (content/bots/<bot>/policy.bin).
//
// forward(): the player's own row (q) attends over every entity (kv: ball, cars, boost pads):
//   q' = relu(L2(relu(L1(q))))            kv'[e] = relu(L2(relu(L1(kv[e]))))
//   per block: x = q' + MultiheadAttention(N1(q'), N2(kv'), N2(kv'));  q' = x + L2(relu(L1(N3(x))))
//     (N = LayerNorm in Nexto's blocks; Necto's have none)
//   logits = Out(relu(q'))  (Necto: one linear layer, split into its heads)
//     or, Nexto: logits[a] = Emb(action a) . Conv(relu(q')) over its action table
// Checked against PyTorch on recorded inputs in tests/scenario_tests.cpp. No SDK calls.
class EarlPolicy
{
public:
	// Reads policy.bin. `heads`: the attention's head count (4 for Necto and Nexto).
	bool load(const std::filesystem::path& file, std::string& error, int heads = 4);
	bool loaded() const { return !blocks_.empty(); }

	int queryDim() const { return q1_.in; }
	int entityDim() const { return kv1_.in; }
	int outputs() const { return actionEmb_.empty() ? out_.out : actions_; }
	// Nexto: its action table (actions() rows of 8 controls), empty for Necto.
	const std::vector<float>& actionTable() const { return actionTable_; }
	int actions() const { return actions_; }

	// `q`: queryDim() values; `kv`: entities x entityDim() values, row by row; `padded`: entities to ignore (may be
	// empty). Returns outputs() logits.
	std::vector<float> forward(const std::vector<float>& q, const std::vector<float>& kv, int entities,
		const std::vector<bool>& padded = {}) const;

private:
	struct Linear
	{
		int in = 0, out = 0;
		std::vector<float> w; // out x in, row-major (PyTorch's layout)
		std::vector<float> b;
		void apply(const float* x, float* y) const;
	};
	struct Norm
	{
		std::vector<float> w, b; // empty = none
		void apply(const float* x, float* y, int n) const;
	};
	struct Block
	{
		Linear q, k, v;   // in_proj split in three
		Linear outProj;
		Linear lin1, lin2;
		Norm n1, n2, n3;  // Nexto
	};

	Linear q1_, q2_, kv1_, kv2_, out_;
	Linear embConv_;                 // Nexto: the state's side of the dot product
	std::vector<float> actionEmb_;   // Nexto: actions_ x embedding, precomputed from its table
	std::vector<float> actionTable_;
	int actions_ = 0;
	int embDim_ = 0;
	std::vector<Block> blocks_;
	int heads_ = 4;
};
