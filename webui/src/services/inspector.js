// Transport adapter used by the React-free Inspector model.
import { api } from '../api'

export const inspectorService = {
  getBinding: (symbolId, options) => api.getBinding(symbolId, options),
  getMatches: (symbolId, options) => api.symbolMatches(symbolId, options),
  searchFootprints: (query, near, options) => api.searchFootprints(query, near, options),
  searchModels: (query, near, options) => api.searchModels(query, near, options),
  bindFootprint: (symbolId, footprintId) => api.bindFootprint(symbolId, footprintId),
  bindModel: (footprintId, modelId) => api.bindModel(footprintId, modelId),
}
