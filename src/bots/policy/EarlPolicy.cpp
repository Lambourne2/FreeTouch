#include "pch.h"
#include "bots/policy/EarlPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>

namespace
{
	struct Tensor
	{
		std::vector<uint32_t> dims;
		std::vector<float> data;
	};

	template <class T>
	bool readPod(std::ifstream& in, T& v)
	{
		return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(T)));
	}
}

void EarlPolicy::Linear::apply(const float* x, float* y) const
{
	for (int o = 0; o < out; ++o)
	{
		const float* row = w.data() + static_cast<size_t>(o) * in;
		float s = b.empty() ? 0.f : b[o];
		for (int i = 0; i < in; ++i)
			s += row[i] * x[i];
		y[o] = s;
	}
}

void EarlPolicy::Norm::apply(const float* x, float* y, int n) const
{
	if (w.empty())
	{
		std::copy(x, x + n, y);
		return;
	}
	double mean = 0.0, var = 0.0;
	for (int i = 0; i < n; ++i)
		mean += x[i];
	mean /= n;
	for (int i = 0; i < n; ++i)
		var += (x[i] - mean) * (x[i] - mean);
	var /= n;
	const float inv = static_cast<float>(1.0 / std::sqrt(var + 1e-5));
	for (int i = 0; i < n; ++i)
		y[i] = (x[i] - static_cast<float>(mean)) * inv * w[i] + b[i];
}

bool EarlPolicy::load(const std::filesystem::path& file, std::string& error, int heads)
{
	actionEmb_.clear();
	actionTable_.clear();
	actions_ = 0;
	blocks_.clear();
	heads_ = heads;
	std::ifstream in(file, std::ios::binary);
	if (!in)
	{
		error = "cannot open " + file.string();
		return false;
	}
	char magic[6];
	uint32_t count = 0;
	if (!in.read(magic, 6) || std::memcmp(magic, "FTPOL1", 6) != 0 || !readPod(in, count))
	{
		error = "not a policy file (run tools/export_policy.py)";
		return false;
	}
	std::map<std::string, Tensor> tensors;
	for (uint32_t t = 0; t < count; ++t)
	{
		uint16_t nameLen = 0;
		uint8_t rank = 0;
		if (!readPod(in, nameLen))
			break;
		std::string name(nameLen, '\0');
		in.read(name.data(), nameLen);
		readPod(in, rank);
		Tensor tensor;
		size_t n = 1;
		for (uint8_t d = 0; d < rank; ++d)
		{
			uint32_t dim = 0;
			readPod(in, dim);
			tensor.dims.push_back(dim);
			n *= dim;
		}
		tensor.data.resize(n);
		in.read(reinterpret_cast<char*>(tensor.data.data()), static_cast<std::streamsize>(n * sizeof(float)));
		if (!in)
		{
			error = "policy file is cut short at " + name;
			return false;
		}
		tensors[name] = std::move(tensor);
	}

	// Necto's TorchScript wraps the network as "net.*"; accept it with or without the prefix.
	auto find = [&](const std::string& name) -> const Tensor* {
		for (const std::string& p : {std::string("net."), std::string()})
		{
			const auto it = tensors.find(p + name);
			if (it != tensors.end())
				return &it->second;
		}
		return nullptr;
	};
	auto linear = [&](const std::string& prefix, Linear& l) {
		const Tensor* w = find(prefix + ".weight");
		const Tensor* b = find(prefix + ".bias");
		if (!w || w->dims.size() != 2)
			return false;
		l.out = static_cast<int>(w->dims[0]);
		l.in = static_cast<int>(w->dims[1]);
		l.w = w->data;
		l.b = b ? b->data : std::vector<float>{};
		return true;
	};
	if (!linear("earl.query_preprocess.0", q1_) || !linear("earl.query_preprocess.2", q2_)
		|| !linear("earl.key_value_preprocess.0", kv1_) || !linear("earl.key_value_preprocess.2", kv2_))
	{
		error = "policy file is missing the preprocess layers";
		return false;
	}
	if (!linear("output.linear", out_))
	{
		// Nexto: a dot product between each action's embedding and the state's.
		Linear a0, a1, a2;
		const Tensor* table = find("output.actions");
		if (!linear("output.emb_convertor", embConv_) || !linear("output.net.0", a0) || !linear("output.net.2", a1)
			|| !linear("output.net.4", a2) || !table || table->dims.size() != 2 || static_cast<int>(table->dims[1]) != a0.in)
		{
			error = "policy file has no output layer it knows";
			return false;
		}
		actions_ = static_cast<int>(table->dims[0]);
		embDim_ = a2.out;
		actionTable_ = table->data;
		actionEmb_.assign(static_cast<size_t>(actions_) * embDim_, 0.f);
		std::vector<float> h0(a0.out), h1(a1.out);
		for (int a = 0; a < actions_; ++a)
		{
			a0.apply(actionTable_.data() + static_cast<size_t>(a) * a0.in, h0.data());
			for (auto& v : h0) v = std::max(0.f, v);
			a1.apply(h0.data(), h1.data());
			for (auto& v : h1) v = std::max(0.f, v);
			float* emb = actionEmb_.data() + static_cast<size_t>(a) * embDim_;
			a2.apply(h1.data(), emb);
			for (int i = 0; i < embDim_; ++i) emb[i] = std::max(0.f, emb[i]);
		}
	}
	for (int i = 0;; ++i)
	{
		const std::string p = "earl.blocks." + std::to_string(i) + ".";
		const Tensor* inW = find(p + "attention.in_proj_weight");
		const Tensor* inB = find(p + "attention.in_proj_bias");
		if (!inW)
			break;
		Block blk;
		const int e = static_cast<int>(inW->dims[1]);
		for (int part = 0; part < 3; ++part)
		{
			Linear& l = part == 0 ? blk.q : part == 1 ? blk.k : blk.v;
			l.in = e;
			l.out = e;
			l.w.assign(inW->data.begin() + static_cast<size_t>(part) * e * e, inW->data.begin() + static_cast<size_t>(part + 1) * e * e);
			if (inB)
				l.b.assign(inB->data.begin() + part * e, inB->data.begin() + (part + 1) * e);
		}
		if (!linear(p + "attention.out_proj", blk.outProj) || !linear(p + "linear1", blk.lin1) || !linear(p + "linear2", blk.lin2))
		{
			error = "policy block " + std::to_string(i) + " is incomplete";
			return false;
		}
		for (auto [name, norm] : {std::pair{"norm1", &blk.n1}, std::pair{"norm2", &blk.n2}, std::pair{"norm3", &blk.n3}})
		{
			const Tensor* w = find(p + name + ".weight");
			const Tensor* b = find(p + name + ".bias");
			if (w && b)
			{
				norm->w = w->data;
				norm->b = b->data;
			}
		}
		blocks_.push_back(std::move(blk));
	}
	if (blocks_.empty() || q2_.out % heads_ != 0)
	{
		error = "policy has no attention blocks (or the head count does not divide its width)";
		return false;
	}
	return true;
}

std::vector<float> EarlPolicy::forward(const std::vector<float>& q, const std::vector<float>& kv, int entities,
	const std::vector<bool>& padded) const
{
	const int e = q2_.out;
	auto relu = [](std::vector<float>& v) {
		for (auto& x : v)
			x = std::max(0.f, x);
	};

	// Preprocess the query row and every entity.
	std::vector<float> hidden(q1_.out), x(e);
	q1_.apply(q.data(), hidden.data());
	relu(hidden);
	q2_.apply(hidden.data(), x.data());
	relu(x);
	std::vector<float> kvx(static_cast<size_t>(entities) * e);
	std::vector<float> kvHidden(kv1_.out);
	for (int n = 0; n < entities; ++n)
	{
		kv1_.apply(kv.data() + static_cast<size_t>(n) * kv1_.in, kvHidden.data());
		relu(kvHidden);
		float* row = kvx.data() + static_cast<size_t>(n) * e;
		kv2_.apply(kvHidden.data(), row);
		for (int i = 0; i < e; ++i)
			row[i] = std::max(0.f, row[i]);
	}

	const int headDim = e / heads_;
	const float scale = 1.f / std::sqrt(static_cast<float>(headDim));
	std::vector<float> qp(e), keys(static_cast<size_t>(entities) * e), values(static_cast<size_t>(entities) * e);
	std::vector<float> attended(e), projected(e), scores(entities), ff, normed(e), kvNormed(e);
	for (const Block& blk : blocks_)
	{
		blk.n1.apply(x.data(), normed.data(), e);
		blk.q.apply(normed.data(), qp.data());
		for (int n = 0; n < entities; ++n)
		{
			blk.n2.apply(kvx.data() + static_cast<size_t>(n) * e, kvNormed.data(), e);
			blk.k.apply(kvNormed.data(), keys.data() + static_cast<size_t>(n) * e);
			blk.v.apply(kvNormed.data(), values.data() + static_cast<size_t>(n) * e);
		}
		for (int h = 0; h < heads_; ++h)
		{
			const int off = h * headDim;
			float best = -std::numeric_limits<float>::infinity();
			for (int n = 0; n < entities; ++n)
			{
				if (n < static_cast<int>(padded.size()) && padded[n])
				{
					scores[n] = -std::numeric_limits<float>::infinity();
					continue;
				}
				float s = 0.f;
				const float* k = keys.data() + static_cast<size_t>(n) * e + off;
				for (int i = 0; i < headDim; ++i)
					s += qp[off + i] * scale * k[i];
				scores[n] = s;
				best = std::max(best, s);
			}
			float sum = 0.f;
			for (int n = 0; n < entities; ++n)
			{
				scores[n] = std::isinf(scores[n]) ? 0.f : std::exp(scores[n] - best);
				sum += scores[n];
			}
			for (int i = 0; i < headDim; ++i)
			{
				float a = 0.f;
				for (int n = 0; n < entities; ++n)
					a += scores[n] * values[static_cast<size_t>(n) * e + off + i];
				attended[off + i] = sum > 0.f ? a / sum : 0.f;
			}
		}
		blk.outProj.apply(attended.data(), projected.data());
		for (int i = 0; i < e; ++i)
			x[i] += projected[i];
		ff.assign(blk.lin1.out, 0.f);
		blk.n3.apply(x.data(), normed.data(), e);
		blk.lin1.apply(normed.data(), ff.data());
		relu(ff);
		blk.lin2.apply(ff.data(), projected.data());
		for (int i = 0; i < e; ++i)
			x[i] += projected[i];
	}
	relu(x);
	if (!actionEmb_.empty())
	{
		std::vector<float> state(embConv_.out);
		embConv_.apply(x.data(), state.data());
		std::vector<float> logits(actions_);
		for (int a = 0; a < actions_; ++a)
		{
			const float* emb = actionEmb_.data() + static_cast<size_t>(a) * embDim_;
			float s = 0.f;
			for (int i = 0; i < embDim_; ++i)
				s += emb[i] * state[i];
			logits[a] = s;
		}
		return logits;
	}
	std::vector<float> logits(out_.out);
	out_.apply(x.data(), logits.data());
	return logits;
}
