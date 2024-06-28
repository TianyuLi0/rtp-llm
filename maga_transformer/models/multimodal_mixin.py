import json
import torch
import re
from functools import partial
from enum import Enum, auto
from typing import Any, Dict, List, Union, Tuple, Optional
from PIL import Image
from decord import VideoReader, cpu

from maga_transformer.config.exceptions import ExceptionType, FtRuntimeException
from maga_transformer.config.generate_config import RequestFormat
from maga_transformer.utils.model_weight import ModelDeployWeightInfo, CkptWeightInfo, WeightInfo, sp_id, identity
from maga_transformer.config.gpt_init_model_parameters import GptInitModelParameters
from maga_transformer.ops.comm.nccl_op import NcclOp
from maga_transformer.distribute.worker_info import g_parallel_info
from maga_transformer.utils.multimodal_util import get_bytes_io_from_url, check_cache, insert_cache
from maga_transformer.models.base_model import EmbeddingOutput

class MultiModalEmbeddingInterface:
    @torch.no_grad()
    def mm_embedding(self, url: str, device):
        cached_res = check_cache(url)
        if cached_res is None:
            try:
                bytes_io = get_bytes_io_from_url(url)
                mm_input = self._mm_preprocess(bytes_io)
            except Exception as e:
                raise Exception(f"cannot download image from {url}, exception {e}")
            features = self.mm_process(mm_input, device)
            insert_cache(url, features)
            return features
        else:
            return cached_res

    def _mm_preprocess(self, data):
        raise NotImplementedError

    @torch.no_grad()
    def mm_process(self, mm_input, device):
        raise NotImplementedError

class ImageEmbeddingInterface(MultiModalEmbeddingInterface):
    def _mm_preprocess(self, data):
        return Image.open(data).convert("RGB")

    @torch.no_grad()
    def mm_process(self, mm_input, device):
        return self.image_embedding([mm_input], device)[0]

    @torch.no_grad()
    def image_embedding(self, images: List[Image.Image], device):
        raise NotImplementedError()

class AudioEmbeddingInterface(MultiModalEmbeddingInterface):
    def _mm_preprocess(self, data):
        # temporary
        import torchaudio
        return torchaudio.load(data)

    @torch.no_grad()
    def mm_process(self, mm_input, device):
        return self.audio_embedding(mm_input, device)

    @torch.no_grad()
    def audio_embedding(self, audio: Tuple[torch.Tensor, int], device):
        raise NotImplementedError()

class VideoEmbeddingInterface(MultiModalEmbeddingInterface):
    def _mm_preprocess(self, data):
        return VideoReader(data, ctx=cpu(0))

    @torch.no_grad()
    def mm_process(self, mm_input, device):
        return self.video_embedding(mm_input, device)

    @torch.no_grad()
    def video_embedding(self, video: List[Image.Image], device):
        raise NotImplementedError()

class BaseVitWeights:
    def __init__(self, vit_part: Dict[str, Any], with_prefix: bool = False):
        self.weight_names: List[str] = []
        self._set_weight_prefix()
        self._get_vit_params(vit_part, with_prefix)

    def _set_weight_prefix(self):
        self._ckpt_prefix = "model."
        self._ft_prefix = "self.mm_part."

    @property
    def ckpt_prefix(self) -> str:
        return self._ckpt_prefix

    @property
    def ft_prefix(self) -> str:
        return self._ft_prefix

    @ft_prefix.setter
    def ft_prefix(self, prefix: str) -> None:
        self._ft_prefix = prefix

    def _get_vit_params(self, vit_part: Dict[str, Any], with_prefix: bool = False):
        for vit_name, vit in vit_part.items():
            if isinstance(vit, torch.nn.Module):
                if len(vit_part) >= 2 or with_prefix:
                    self.weight_names.extend([vit_name + '.' + w for w in vit.state_dict().keys()])
                else:
                    self.weight_names.extend(list(vit.state_dict().keys()))
            elif isinstance(vit, torch.nn.Parameter):
                self.weight_names.append(vit_name)
            else:
                raise Exception("Unknown vit part type")

class BaseMultiModalWeightInfo:
    def __init__(self, config: GptInitModelParameters):
        self.vit_weights: Optional[BaseVitWeights] = config.vit_related_params.vit_weights

    def _get_vit_info(self, llm_weights: ModelDeployWeightInfo):
        if self.vit_weights is not None:
            weight_names = self.vit_weights.weight_names
            ckpt_prefix = self.vit_weights.ckpt_prefix

            for w in weight_names:
                w_name = ckpt_prefix + w
                llm_weights.weights.append(WeightInfo(w_name, [CkptWeightInfo(w_name, identity)], identity))
                llm_weights.tp_strategy[w_name] = sp_id

class MultiModalMixin:
    mm_part: MultiModalEmbeddingInterface
    nccl_op_: NcclOp

    @staticmethod
    def process_encode_plugin(prompt: str, generate_config: Dict[str, Any], tokenizer: Any, add_special_tokens: bool, **kwargs: Any) -> List[int]:
        if len(prompt) == 0:
            raise FtRuntimeException(ExceptionType.EMPTY_PROMPT_ERROR, "prompt should have at least one token!")
        if type(prompt) is not str:
            raise FtRuntimeException(ExceptionType.ERROR_INPUT_FORMAT_ERROR, "expect string prompt, actual: " + str(prompt))
        if add_special_tokens:
            return tokenizer.encode(prompt)
        else:
            # for CogVLM2, we need to pass add_special_tokens=False to tokenizer
            return tokenizer.encode(prompt, add_special_tokens=False)

    @staticmethod
    def multimodal_modify_prompt_plugin(prompt: Union[List[Dict[str, Any]], str], images: List[str],
                                        img_token: str, **kwargs: Any) -> Tuple[str, List[str]]:
        # should delete after chatapi interface update
        if kwargs.get('generate_config', {})['request_format'] == RequestFormat.CHAT_API:
            if isinstance(prompt, str):
                messages = json.loads(prompt, strict=False)
            else:
                messages = prompt
            new_prompt: str = ""
            new_images: List[str] = []
            for message in messages:
                new_prompt += message['role'].upper() + ' :'
                if isinstance(message['content'], str):
                    new_prompt += message['content'] + '\n'
                elif isinstance(message['content'], List):
                    for x in message['content']:
                        if x['type'] == 'text':
                            new_prompt += x['text']
                        elif x['type'] == 'image_url':
                            now_images = x['image_url']
                            if isinstance(now_images, List):
                                new_images.extend(now_images)
                                new_prompt += (img_token + '\n') * len(now_images)
                            else:
                                new_images.append(now_images)
                                new_prompt += img_token + '\n'
                        else:
                            raise FtRuntimeException(ExceptionType.ERROR_INPUT_FORMAT_ERROR, "content type can only be text or image_url, but get: " + x['type'])
                    new_prompt += '\n'
            return new_prompt + 'ASSISTANT :', new_images
        elif isinstance(prompt, List):
            raise FtRuntimeException(ExceptionType.ERROR_INPUT_FORMAT_ERROR, "raw request format cannot accept dict prompt")
        return prompt, images

    def expand_token_id(self, token_ids: List[int], images: List[torch.Tensor]) -> Tuple[List[int], List[torch.Tensor], List[int]]:
        raise NotImplementedError()

    def load_vit_weight(self, ctype: str):
        vit_weight = self.config.vit_related_params.vit_weights
        ckpt_prefix = vit_weight.ckpt_prefix
        ft_prefix = vit_weight.ft_prefix
        weight_names = vit_weight.weight_names

        def _safe_load_from_module(param: torch.nn.Parameter, fname: str, ctype: torch.dtype):
            param.data = self.weight.steal_pytorch_weight(fname).reshape(param.data.shape).to(ctype).to('cuda:0')

        for w in weight_names:
            w_name = ft_prefix + w
            w_name = re.sub(r'\.\d+\.', lambda x: '[' + x.group(0)[1:-1] + '].', w_name)
            param = eval(w_name)
            _safe_load_from_module(param, ckpt_prefix + w, ctype)

    def async_input_word_embedding(self, inputs: torch.Tensor, images: List[torch.Tensor], token_type_ids: torch.Tensor):
        inputs = inputs.reshape(1, -1)
        if g_parallel_info.tp_size <= 1:
            return EmbeddingOutput(self.multimodal_embedding(inputs, images, token_type_ids).squeeze(0), None)

        if g_parallel_info.tp_rank == 0:
            embedding_tensor = self.multimodal_embedding(inputs, images, token_type_ids).squeeze(0)
        else:
            embedding_tensor = torch.zeros((inputs.shape[1], self.config.head_num * self.config.size_per_head), dtype=self.dtype, device=self.device)
        self.nccl_op_.broadcast_tp([embedding_tensor])
        torch.cuda.current_stream().synchronize()
        return EmbeddingOutput(embedding_tensor, None)
